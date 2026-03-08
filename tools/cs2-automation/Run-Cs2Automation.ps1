[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ScenarioPath,

    [string]$HlaePath,

    [string]$OutputRoot,

    [int]$StartupTimeoutSeconds = 180,

    [int]$TimeoutSeconds = 0,

    [switch]$SkipWindowPlacement
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class Cs2AutomationWin32
{
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(
        IntPtr hWnd,
        IntPtr hWndInsertAfter,
        int X,
        int Y,
        int cx,
        int cy,
        uint uFlags
    );
}
"@

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if (-not $HlaePath) {
    $HlaePath = Join-Path $RepoRoot "build\Release\dist\bin\HLAE.exe"
}

if (-not $OutputRoot) {
    $OutputRoot = Join-Path $RepoRoot "build\cs2-automation"
}

function Get-FullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return [System.IO.Path]::GetFullPath($Path)
}

function Convert-ToConsolePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return (Get-FullPath -Path $Path).Replace("\", "/")
}

function Write-WrapperLog {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-ddTHH:mm:ss"), $Message
    Write-Host $line
    Add-Content -Path $script:WrapperLogPath -Value $line
}

function Expand-Tokens {
    param(
        [AllowNull()]
        [string]$Value,

        [Parameter(Mandatory = $true)]
        [hashtable]$Tokens
    )

    if ($null -eq $Value) {
        return $null
    }

    $result = $Value
    foreach ($key in $Tokens.Keys) {
        $result = $result.Replace(("{{{{{0}}}}}" -f $key), [string]$Tokens[$key])
    }

    return $result
}

function Get-ScenarioPropertyValue {
    param(
        [Parameter(Mandatory = $true)]
        [psobject]$Scenario,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        $Default = $null
    )

    $property = $Scenario.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $Default
    }

    return $property.Value
}

function Resolve-DemoLaunchName {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DemoPath,

        [Parameter(Mandatory = $true)]
        [string]$Cs2Exe
    )

    $demoFullPath = Get-FullPath -Path $DemoPath
    $gameDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Get-FullPath -Path $Cs2Exe)))
    $csgoDir = Join-Path $gameDir "csgo"

    $comparison = [System.StringComparison]::OrdinalIgnoreCase
    $trimmedCsgoDir = $csgoDir.TrimEnd('\')
    if ($demoFullPath.StartsWith($trimmedCsgoDir, $comparison)) {
        $relativePath = $demoFullPath.Substring($trimmedCsgoDir.Length).TrimStart('\')
        if ($relativePath.EndsWith(".dem", $comparison)) {
            $relativePath = $relativePath.Substring(0, $relativePath.Length - 4)
        }
        return $relativePath.Replace("\", "/")
    }

    if ($demoFullPath.EndsWith(".dem", $comparison)) {
        $demoFullPath = $demoFullPath.Substring(0, $demoFullPath.Length - 4)
    }

    return $demoFullPath.Replace("\", "/")
}

function New-CommandSystemXml {
    param(
        [Parameter(Mandatory = $true)]
        [array]$TickCommands,

        [Parameter(Mandatory = $true)]
        [hashtable]$Tokens,

        [Parameter(Mandatory = $true)]
        [string]$CommandSystemPath
    )

    $doc = New-Object System.Xml.XmlDocument
    $declaration = $doc.CreateXmlDeclaration("1.0", "utf-8", $null)
    [void]$doc.AppendChild($declaration)

    $root = $doc.CreateElement("commandSystem")
    [void]$doc.AppendChild($root)

    $commandsNode = $doc.CreateElement("commands")
    [void]$root.AppendChild($commandsNode)

    foreach ($entry in ($TickCommands | Sort-Object { [double]$_.tick })) {
        $commandNode = $doc.CreateElement("c")
        [void]$commandNode.SetAttribute(
            "tick",
            [string]::Format([System.Globalization.CultureInfo]::InvariantCulture, "{0}", [double]$entry.tick)
        )

        $bodyNode = $doc.CreateElement("body")
        $bodyNode.InnerText = Expand-Tokens -Value ([string]$entry.command) -Tokens $Tokens
        [void]$commandNode.AppendChild($bodyNode)
        [void]$commandsNode.AppendChild($commandNode)
    }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $writerSettings = New-Object System.Xml.XmlWriterSettings
    $writerSettings.Encoding = $utf8NoBom
    $writerSettings.Indent = $true

    $writer = [System.Xml.XmlWriter]::Create($CommandSystemPath, $writerSettings)
    try {
        $doc.Save($writer)
    }
    finally {
        $writer.Dispose()
    }
}

function Wait-ForNewProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProcessName,

        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)]
        [int[]]$ExistingIds,

        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$LauncherProcess,

        [Parameter(Mandatory = $true)]
        [datetime]$Deadline
    )

    do {
        $candidate = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
            Where-Object { $ExistingIds -notcontains $_.Id } |
            Sort-Object StartTime -Descending |
            Select-Object -First 1

        if ($candidate) {
            return $candidate
        }

        $LauncherProcess.Refresh()
        if ($LauncherProcess.HasExited) {
            throw ("Launcher process exited before {0} started. Exit code: {1}" -f $ProcessName, $LauncherProcess.ExitCode)
        }

        Start-Sleep -Seconds 1
    } while ((Get-Date) -lt $Deadline)

    throw "Timed out waiting for a new $ProcessName process."
}

function Wait-ForMainWindowHandle {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId,

        [Parameter(Mandatory = $true)]
        [datetime]$Deadline
    )

    do {
        $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
        if (-not $process) {
            throw "Process $ProcessId exited before its main window became available."
        }

        $process.Refresh()
        if ($process.MainWindowHandle -ne 0) {
            return [IntPtr]$process.MainWindowHandle
        }

        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $Deadline)

    throw "Timed out waiting for the CS2 window handle."
}

function Connect-NetCon {
    param(
        [Parameter(Mandatory = $true)]
        [int]$Port,

        [Parameter(Mandatory = $true)]
        [datetime]$Deadline
    )

    do {
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $client.Connect("127.0.0.1", $Port)
            return $client
        }
        catch {
            $client.Dispose()
        }

        Start-Sleep -Seconds 1
    } while ((Get-Date) -lt $Deadline)

    throw "Timed out waiting for CS2 netcon on port $Port."
}

function Read-NetConData {
    param(
        [Parameter(Mandatory = $true)]
        [System.Net.Sockets.TcpClient]$Client,

        [Parameter(Mandatory = $true)]
        [string]$LogPath
    )

    if (-not $Client.Connected) {
        return ""
    }

    $stream = $Client.GetStream()
    $encoding = [System.Text.Encoding]::ASCII
    $buffer = New-Object byte[] 8192
    $builder = New-Object System.Text.StringBuilder

    while ($stream.DataAvailable) {
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -le 0) {
            break
        }

        $text = $encoding.GetString($buffer, 0, $read)
        [void]$builder.Append($text)
        Add-Content -Path $LogPath -Value $text -NoNewline
        Start-Sleep -Milliseconds 50
    }

    return $builder.ToString()
}

function Send-NetConCommand {
    param(
        [Parameter(Mandatory = $true)]
        [System.Net.Sockets.TcpClient]$Client,

        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter(Mandatory = $true)]
        [string]$LogPath
    )

    if (-not $Client.Connected) {
        throw "CS2 netcon connection is not available."
    }

    $stream = $Client.GetStream()
    $encoding = [System.Text.Encoding]::ASCII
    $payload = $encoding.GetBytes($Command + "`n")
    $stream.Write($payload, 0, $payload.Length)
    $stream.Flush()
    Start-Sleep -Milliseconds 250
    return (Read-NetConData -Client $Client -LogPath $LogPath)
}

function Invoke-NetConCommandWithMarker {
    param(
        [Parameter(Mandatory = $true)]
        [System.Net.Sockets.TcpClient]$Client,

        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter(Mandatory = $true)]
        [string]$LogPath,

        [int]$TimeoutMilliseconds = 4000
    )

    $marker = "AIAUTO:MARKER:{0}" -f ([Guid]::NewGuid().ToString("N"))
    $stream = $Client.GetStream()
    $encoding = [System.Text.Encoding]::ASCII
    $payload = $encoding.GetBytes($Command + "`n" + "echo " + $marker + "`n")
    $stream.Write($payload, 0, $payload.Length)
    $stream.Flush()

    $deadline = (Get-Date).AddMilliseconds($TimeoutMilliseconds)
    $captured = ""
    do {
        Start-Sleep -Milliseconds 100
        $captured += Read-NetConData -Client $Client -LogPath $LogPath
        if ($captured.Contains($marker)) {
            return $captured
        }
    } while ((Get-Date) -lt $deadline)

    return $captured
}

function Wait-ForMirvCommand {
    param(
        [Parameter(Mandatory = $true)]
        [System.Net.Sockets.TcpClient]$Client,

        [Parameter(Mandatory = $true)]
        [string]$LogPath,

        [Parameter(Mandatory = $true)]
        [datetime]$Deadline
    )

    do {
        $response = Invoke-NetConCommandWithMarker -Client $Client -Command "mirv_cmd print" -LogPath $LogPath
        if ($response -and $response -notmatch "Unknown command 'mirv_cmd'!" -and $response -match "mirv_cmd|Command system") {
            return
        }

        Start-Sleep -Seconds 1
    } while ((Get-Date) -lt $Deadline)

    throw "Timed out waiting for HLAE CS2 commands to become available."
}

function Move-ToAutomationScreen {
    param(
        [Parameter(Mandatory = $true)]
        [IntPtr]$WindowHandle,

        [Parameter(Mandatory = $true)]
        [int]$ScreenIndex,

        [Parameter(Mandatory = $true)]
        [int]$Width,

        [Parameter(Mandatory = $true)]
        [int]$Height
    )

    $screens = [System.Windows.Forms.Screen]::AllScreens
    if ($screens.Count -eq 0) {
        Write-WrapperLog "No screens reported by WinForms, skipping window placement."
        return
    }

    if ($ScreenIndex -lt 0 -or $ScreenIndex -ge $screens.Count) {
        $fallback = $screens | Where-Object { -not $_.Primary } | Select-Object -First 1
        if (-not $fallback) {
            $fallback = $screens[0]
        }

        $targetScreen = $fallback
        Write-WrapperLog ("Requested screen index {0} is unavailable, using screen '{1}' instead." -f $ScreenIndex, $targetScreen.DeviceName)
    }
    else {
        $targetScreen = $screens[$ScreenIndex]
    }

    $bounds = $targetScreen.WorkingArea
    [void][Cs2AutomationWin32]::ShowWindowAsync($WindowHandle, 4)
    [void][Cs2AutomationWin32]::SetWindowPos(
        $WindowHandle,
        [IntPtr]::Zero,
        $bounds.Left,
        $bounds.Top,
        $Width,
        $Height,
        0x0040 -bor 0x0010
    )

    Write-WrapperLog (
        ("Placed CS2 window on screen '{0}' at {1},{2} with size {3}x{4}." `
            -f $targetScreen.DeviceName, $bounds.Left, $bounds.Top, $Width, $Height)
    )
}

function Resolve-ArtifactPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactPath,

        [Parameter(Mandatory = $true)]
        [string]$OutputDir
    )

    if ([System.IO.Path]::IsPathRooted($ArtifactPath)) {
        return Get-FullPath -Path $ArtifactPath
    }

    return Get-FullPath -Path (Join-Path $OutputDir $ArtifactPath)
}

function Format-ProcessArgument {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Argument
    )

    if ($Argument -eq "") {
        return '""'
    }

    if ($Argument -notmatch '[\s"]') {
        return $Argument
    }

    return '"' + ($Argument -replace '"', '\"') + '"'
}

$scenarioFile = Get-FullPath -Path $ScenarioPath
$scenario = Get-Content -Path $scenarioFile -Raw | ConvertFrom-Json

if (-not $scenario.name) {
    throw "Scenario '$scenarioFile' is missing the required 'name' property."
}

if (-not $scenario.cs2Exe) {
    throw "Scenario '$scenarioFile' is missing the required 'cs2Exe' property."
}

if (-not $scenario.demoPath) {
    throw "Scenario '$scenarioFile' is missing the required 'demoPath' property."
}

$HlaePath = Get-FullPath -Path $HlaePath
if (-not (Test-Path -LiteralPath $HlaePath)) {
    throw "HLAE executable not found: $HlaePath"
}

$cs2Exe = Get-FullPath -Path ([string]$scenario.cs2Exe)
if (-not (Test-Path -LiteralPath $cs2Exe)) {
    throw "CS2 executable not found: $cs2Exe"
}

$demoPath = Get-FullPath -Path ([string]$scenario.demoPath)
if (-not (Test-Path -LiteralPath $demoPath)) {
    throw "Demo file not found: $demoPath"
}

$existingCs2 = @(Get-Process -Name "cs2" -ErrorAction SilentlyContinue)
if ($existingCs2.Count -gt 0) {
    throw "CS2 is already running. Close it before starting the automation pipeline."
}

$runName = "{0}-{1:yyyyMMdd-HHmmss}" -f [string]$scenario.name, (Get-Date)
$outputDir = Get-FullPath -Path (Join-Path $OutputRoot $runName)
$mmcfgRoot = Join-Path $outputDir "mmcfg"
$cfgDir = Join-Path $mmcfgRoot "cfg"
$logsDir = Join-Path $outputDir "logs"
$artifactsDir = Join-Path $outputDir "artifacts"

New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
New-Item -ItemType Directory -Force -Path $logsDir | Out-Null
New-Item -ItemType Directory -Force -Path $artifactsDir | Out-Null

$script:WrapperLogPath = Join-Path $logsDir "automation.log"
New-Item -ItemType File -Force -Path $script:WrapperLogPath | Out-Null

$commandSystemPath = Join-Path $outputDir "command-system.xml"
$bootstrapCfgName = "autoexec.cfg"
$bootstrapCfgPath = Join-Path $cfgDir $bootstrapCfgName
$consoleLogPath = Join-Path $logsDir "cs2-console.log"
$resultPath = Join-Path $outputDir "result.json"

$tokens = @{
    scenarioName = [string]$scenario.name
    outputDir = Get-FullPath -Path $outputDir
    outputDirForward = Convert-ToConsolePath -Path $outputDir
    artifactsDir = Get-FullPath -Path $artifactsDir
    artifactsDirForward = Convert-ToConsolePath -Path $artifactsDir
    consoleLogPath = Get-FullPath -Path $consoleLogPath
    consoleLogPathForward = Convert-ToConsolePath -Path $consoleLogPath
    commandSystemPath = Get-FullPath -Path $commandSystemPath
    commandSystemPathForward = Convert-ToConsolePath -Path $commandSystemPath
    bootstrapCfgPath = Get-FullPath -Path $bootstrapCfgPath
    bootstrapCfgPathForward = Convert-ToConsolePath -Path $bootstrapCfgPath
    demoPath = Get-FullPath -Path $demoPath
    demoPathForward = Convert-ToConsolePath -Path $demoPath
}

$tickCommands = @()
$scenarioTickCommands = Get-ScenarioPropertyValue -Scenario $scenario -Name "tickCommands"
if ($scenarioTickCommands) {
    $tickCommands = @($scenarioTickCommands)
}

New-CommandSystemXml -TickCommands $tickCommands -Tokens $tokens -CommandSystemPath $commandSystemPath

$demoLaunchName = Resolve-DemoLaunchName -DemoPath $demoPath -Cs2Exe $cs2Exe

$bootstrapCommands = @(
    'con_enable "1"',
    'developer "1"',
    ('echo AIAUTO:BOOTSTRAP:{0}' -f $scenario.name),
    'mirv_cmd clear',
    ('mirv_cmd load "{0}"' -f $tokens.commandSystemPathForward)
)

$scenarioBootstrapCommands = Get-ScenarioPropertyValue -Scenario $scenario -Name "bootstrapCommands"
if ($scenarioBootstrapCommands) {
    foreach ($command in $scenarioBootstrapCommands) {
        $bootstrapCommands += (Expand-Tokens -Value ([string]$command) -Tokens $tokens)
    }
}

$bootstrapCommands += ('playdemo "{0}"' -f $demoLaunchName)

$bootstrapContent = ($bootstrapCommands -join [Environment]::NewLine) + [Environment]::NewLine
[System.IO.File]::WriteAllText($bootstrapCfgPath, $bootstrapContent, (New-Object System.Text.UTF8Encoding($false)))

$effectiveScreenIndex = [int](Get-ScenarioPropertyValue -Scenario $scenario -Name "screenIndex" -Default 1)
$effectiveWidth = [int](Get-ScenarioPropertyValue -Scenario $scenario -Name "windowWidth" -Default 854)
$effectiveHeight = [int](Get-ScenarioPropertyValue -Scenario $scenario -Name "windowHeight" -Default 480)
$effectiveStartupTimeout = [int](Get-ScenarioPropertyValue -Scenario $scenario -Name "startupTimeoutSeconds" -Default $StartupTimeoutSeconds)
$scenarioTimeoutSeconds = Get-ScenarioPropertyValue -Scenario $scenario -Name "timeoutSeconds"
$effectiveTimeout = if ($TimeoutSeconds -gt 0) { $TimeoutSeconds } elseif ($null -ne $scenarioTimeoutSeconds) { [int]$scenarioTimeoutSeconds } else { 300 }
$netConPort = [int](Get-ScenarioPropertyValue -Scenario $scenario -Name "netConPort" -Default 2121)

$customLaunchOptions = @("-console", "-novid", "-netconport", [string]$netConPort, "-afxFixNetCon", "+sv_lan", "1")
$scenarioLaunchOptions = Get-ScenarioPropertyValue -Scenario $scenario -Name "launchOptions"
if ($null -ne $scenarioLaunchOptions -and "" -ne [string]$scenarioLaunchOptions) {
    $customLaunchOptions += [string]$scenarioLaunchOptions
}
$customLaunchOptionsText = $customLaunchOptions -join " "

$hlaeArgs = @(
    "-noConfig",
    "-cs2Launcher",
    "-autoStart",
    "-noGui",
    "-cs2Exe", $cs2Exe,
    "-mmcfgEnabled", "true",
    "-mmcfg", $mmcfgRoot,
    "-gfxEnabled", "true",
    "-gfxWidth", [string]$effectiveWidth,
    "-gfxHeight", [string]$effectiveHeight,
    "-gfxFull", "false",
    "-avoidVac", "true",
    "-customLaunchOptions", $customLaunchOptionsText
)
$hlaeArgumentLine = ($hlaeArgs | ForEach-Object { Format-ProcessArgument -Argument ([string]$_) }) -join " "

$foregroundWindow = [Cs2AutomationWin32]::GetForegroundWindow()
$existingIds = @(Get-Process -Name "cs2" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)

$cs2Process = $null
$netConClient = $null
$result = $null

try {
    Write-WrapperLog ("Using scenario file: {0}" -f $scenarioFile)
    Write-WrapperLog ("Generated output directory: {0}" -f $outputDir)
    Write-WrapperLog ("Launching HLAE: {0}" -f $HlaePath)
    Write-WrapperLog ("CS2 launch options: {0}" -f $customLaunchOptionsText)

    $hlaeProcess = Start-Process -FilePath $HlaePath -ArgumentList $hlaeArgumentLine -PassThru -WindowStyle Hidden
    Write-WrapperLog ("Started HLAE process with PID {0}." -f $hlaeProcess.Id)

    $startupDeadline = (Get-Date).AddSeconds($effectiveStartupTimeout)
    $cs2Process = Wait-ForNewProcess -ProcessName "cs2" -ExistingIds $existingIds -LauncherProcess $hlaeProcess -Deadline $startupDeadline
    Write-WrapperLog ("Detected CS2 process with PID {0}." -f $cs2Process.Id)

    if (-not $SkipWindowPlacement) {
        $windowHandle = Wait-ForMainWindowHandle -ProcessId $cs2Process.Id -Deadline $startupDeadline
        Move-ToAutomationScreen -WindowHandle $windowHandle -ScreenIndex $effectiveScreenIndex -Width $effectiveWidth -Height $effectiveHeight

        if ($foregroundWindow -ne [IntPtr]::Zero) {
            [void][Cs2AutomationWin32]::SetForegroundWindow($foregroundWindow)
            Write-WrapperLog "Restored focus to the previously active window."
        }
    }

    $netConClient = Connect-NetCon -Port $netConPort -Deadline $startupDeadline
    Write-WrapperLog ("Connected to CS2 netcon on port {0}." -f $netConPort)
    [void](Read-NetConData -Client $netConClient -LogPath $consoleLogPath)
    Wait-ForMirvCommand -Client $netConClient -LogPath $consoleLogPath -Deadline $startupDeadline
    Write-WrapperLog "HLAE CS2 commands are available over netcon."

    foreach ($command in $bootstrapCommands) {
        [void](Send-NetConCommand -Client $netConClient -Command $command -LogPath $consoleLogPath)
    }

    $runDeadline = (Get-Date).AddSeconds($effectiveTimeout)
    while (-not $cs2Process.HasExited) {
        if ((Get-Date) -ge $runDeadline) {
            throw "Timed out waiting for CS2 to finish after $effectiveTimeout seconds."
        }

        if ($netConClient -and $netConClient.Connected) {
            [void](Read-NetConData -Client $netConClient -LogPath $consoleLogPath)
        }

        Start-Sleep -Seconds 1
        $cs2Process.Refresh()
    }

    if ($netConClient) {
        [void](Read-NetConData -Client $netConClient -LogPath $consoleLogPath)
        $netConClient.Dispose()
        $netConClient = $null
    }

    $cs2ExitCode = $null
    try {
        $cs2ExitCode = $cs2Process.ExitCode
    }
    catch {
    }

    $cs2ExitCodeText = if ($null -ne $cs2ExitCode) { [string]$cs2ExitCode } else { "unknown" }
    Write-WrapperLog ("CS2 exited with code {0}." -f $cs2ExitCodeText)

    $logText = ""
    if (Test-Path -LiteralPath $consoleLogPath) {
        $logText = Get-Content -Path $consoleLogPath -Raw
    }
    else {
        Write-WrapperLog "Console log file was not created."
    }

    $patternResults = @()
    $allPatternsMatched = $true
    foreach ($pattern in @((Get-ScenarioPropertyValue -Scenario $scenario -Name "requiredLogPatterns" -Default @()))) {
        $expandedPattern = Expand-Tokens -Value ([string]$pattern) -Tokens $tokens
        $matched = [System.Text.RegularExpressions.Regex]::IsMatch($logText, $expandedPattern)
        if (-not $matched) {
            $allPatternsMatched = $false
        }

        $patternResults += [PSCustomObject]@{
            pattern = $expandedPattern
            matched = $matched
        }
    }

    $artifactResults = @()
    $allArtifactsValid = $true
    foreach ($artifact in @((Get-ScenarioPropertyValue -Scenario $scenario -Name "expectedArtifacts" -Default @()))) {
        $artifactPathValue = Expand-Tokens -Value ([string]$artifact.path) -Tokens $tokens
        $artifactPath = Resolve-ArtifactPath -ArtifactPath $artifactPathValue -OutputDir $outputDir
        $minBytes = if ($artifact.minBytes) { [int64]$artifact.minBytes } else { 1 }
        $exists = Test-Path -LiteralPath $artifactPath
        $size = if ($exists) { (Get-Item -LiteralPath $artifactPath).Length } else { 0 }
        $valid = $exists -and ($size -ge $minBytes)
        if (-not $valid) {
            $allArtifactsValid = $false
        }

        $artifactResults += [PSCustomObject]@{
            path = $artifactPath
            exists = $exists
            size = $size
            minBytes = $minBytes
            valid = $valid
        }
    }

    $success = $allPatternsMatched -and $allArtifactsValid -and (($null -eq $cs2ExitCode) -or ($cs2ExitCode -eq 0))

    $result = [PSCustomObject]@{
        success = $success
        scenarioName = [string]$scenario.name
        scenarioPath = $scenarioFile
        outputDir = $outputDir
        consoleLogPath = $consoleLogPath
        commandSystemPath = $commandSystemPath
        bootstrapCfgPath = $bootstrapCfgPath
        cs2Pid = $cs2Process.Id
        cs2ExitCode = $cs2ExitCode
        logPatterns = $patternResults
        artifacts = $artifactResults
    }

    $result | ConvertTo-Json -Depth 6 | Set-Content -Path $resultPath -Encoding UTF8

    if (-not $success) {
        Write-WrapperLog "Automation verification failed. See result.json and the console log for details."
        exit 1
    }

    Write-WrapperLog "Automation verification succeeded."
    exit 0
}
catch {
    Write-WrapperLog ("Automation failed: {0}" -f $_.Exception.Message)

    if ($cs2Process -and -not $cs2Process.HasExited) {
        Write-WrapperLog ("Stopping CS2 process {0} after failure." -f $cs2Process.Id)
        Stop-Process -Id $cs2Process.Id -Force
    }

    if ($netConClient) {
        $netConClient.Dispose()
    }

    if ($null -eq $result) {
        [PSCustomObject]@{
            success = $false
            scenarioName = [string]$scenario.name
            scenarioPath = $scenarioFile
            outputDir = $outputDir
            error = $_.Exception.Message
        } | ConvertTo-Json -Depth 4 | Set-Content -Path $resultPath -Encoding UTF8
    }

    throw
}
