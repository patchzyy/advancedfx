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

function Get-PrintableStrings {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [int]$MinLength = 4
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $builder = New-Object System.Text.StringBuilder
    $results = New-Object System.Collections.Generic.List[string]

    foreach ($value in $bytes) {
        if ($value -ge 32 -and $value -le 126) {
            [void]$builder.Append([char]$value)
            continue
        }

        if ($builder.Length -ge $MinLength) {
            $results.Add($builder.ToString())
        }

        [void]$builder.Clear()
    }

    if ($builder.Length -ge $MinLength) {
        $results.Add($builder.ToString())
    }

    return $results.ToArray()
}

function Read-NullTerminatedString {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.BinaryReader]$Reader
    )

    $bytes = New-Object System.Collections.Generic.List[byte]
    while ($Reader.BaseStream.Position -lt $Reader.BaseStream.Length) {
        $value = $Reader.ReadByte()
        if (0 -eq $value) {
            break
        }

        $bytes.Add($value)
    }

    return [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
}

function Read-AgrDictionaryToken {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.BinaryReader]$Reader,

        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[string]]$Dictionary
    )

    $index = $Reader.ReadInt32()
    if ($index -eq -1) {
        $value = Read-NullTerminatedString -Reader $Reader
        $Dictionary.Add($value)
        return $value
    }

    if ($index -lt 0 -or $index -ge $Dictionary.Count) {
        throw "AGR dictionary index out of range: $index"
    }

    return $Dictionary[$index]
}

function Test-AgrNumericExpectation {
    param(
        [Parameter(Mandatory = $true)]
        [double]$Actual,

        [Parameter(Mandatory = $true)]
        $Expected,

        $Summary = $null
    )

    if ($Expected -is [System.Management.Automation.PSCustomObject] -or $Expected -is [hashtable]) {
        $operator = [string](Get-ScenarioPropertyValue -Scenario $Expected -Name "operator" -Default "eq")
        $valueFromProperty = [string](Get-ScenarioPropertyValue -Scenario $Expected -Name "valueFromProperty" -Default "")
        $tolerance = [double](Get-ScenarioPropertyValue -Scenario $Expected -Name "tolerance" -Default 0)
        $value = if ($valueFromProperty -and $Summary) {
            [double]$Summary.$valueFromProperty
        }
        else {
            [double](Get-ScenarioPropertyValue -Scenario $Expected -Name "value" -Default 0)
        }

        switch ($operator) {
            "gt" { return $Actual -gt $value }
            "ge" { return $Actual -ge $value }
            "lt" { return $Actual -lt $value }
            "le" { return $Actual -le $value }
            "ne" { return $Actual -ne $value }
            "approx" { return [math]::Abs($Actual - $value) -le $tolerance }
            default { return $Actual -eq $value }
        }
    }

    return $Actual -eq [double]$Expected
}

function Test-AgrVectorChanged {
    param(
        [double[]]$Current,
        [double[]]$Reference,
        [double]$Tolerance = 0.01
    )

    if ($null -eq $Current -or $null -eq $Reference) {
        return $false
    }

    for ($i = 0; $i -lt [Math]::Min($Current.Length, $Reference.Length); ++$i) {
        if ([math]::Abs([double]$Current[$i] - [double]$Reference[$i]) -gt $Tolerance) {
            return $true
        }
    }

    return $false
}

function Read-AgrFileSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $reader = New-Object System.IO.BinaryReader($stream, [System.Text.Encoding]::UTF8, $true)
        try {
            $signature = Read-NullTerminatedString -Reader $reader
            if ($signature -ne "afxGameRecord") {
                throw "Unexpected AGR signature '$signature'."
            }

            $version = $reader.ReadInt32()
            $dictionary = New-Object 'System.Collections.Generic.List[string]'
            $entityHandles = New-Object System.Collections.Generic.HashSet[int]
            $hiddenHandles = New-Object System.Collections.Generic.HashSet[int]
            $deletedHandles = New-Object System.Collections.Generic.HashSet[int]
            $movingEntityHandles = New-Object System.Collections.Generic.HashSet[int]
            $movingBoneHandles = New-Object System.Collections.Generic.HashSet[int]
            $movingViewModelHandles = New-Object System.Collections.Generic.HashSet[int]
            $worldWeaponHandles = New-Object System.Collections.Generic.HashSet[int]
            $viewModelHandles = New-Object System.Collections.Generic.HashSet[int]
            $entityOriginByHandle = @{}
            $firstBoneTranslationByHandle = @{}
            $summary = [ordered]@{
                version = $version
                frameCount = 0
                entityCount = 0
                uniqueEntityHandleCount = 0
                visibleEntityCount = 0
                invisibleEntityCount = 0
                viewModelEntityCount = 0
                playerCameraCount = 0
                mainCameraCount = 0
                deletedCount = 0
                uniqueDeletedHandleCount = 0
                hiddenCount = 0
                uniqueHiddenHandleCount = 0
                boneEntityCount = 0
                boneCount = 0
                maxBoneCount = 0
                movingEntityHandleCount = 0
                movingBoneEntityCount = 0
                movingViewModelHandleCount = 0
                worldWeaponEntityCount = 0
                uniqueWorldWeaponHandleCount = 0
                worldWeaponModelCount = 0
                uniqueViewModelHandleCount = 0
                viewModelModelCount = 0
                viewModelBoneEntityCount = 0
                firstMainCameraPosX = $null
                firstMainCameraPosY = $null
                firstMainCameraPosZ = $null
                firstMainCameraAng0 = $null
                firstMainCameraAng1 = $null
                firstMainCameraAng2 = $null
                firstMainCameraFov = $null
                lastMainCameraPosX = $null
                lastMainCameraPosY = $null
                lastMainCameraPosZ = $null
                lastMainCameraAng0 = $null
                lastMainCameraAng1 = $null
                lastMainCameraAng2 = $null
                lastMainCameraFov = $null
                firstPlayerCameraFov = $null
                lastPlayerCameraFov = $null
                playerCameraFovMin = $null
                playerCameraFovMax = $null
                models = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)
                viewModelModels = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)
                worldWeaponModels = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)
            }

            while ($reader.BaseStream.Position -lt $reader.BaseStream.Length) {
                $token = Read-AgrDictionaryToken -Reader $reader -Dictionary $dictionary
                switch ($token) {
                    "afxFrame" {
                        [void]$reader.ReadSingle()
                        [void]$reader.ReadInt32()
                        $summary.frameCount++
                    }
                    "afxFrameEnd" {
                    }
                    "afxHidden" {
                        $hiddenCount = $reader.ReadInt32()
                        $summary.hiddenCount += $hiddenCount
                        for ($i = 0; $i -lt $hiddenCount; ++$i) {
                            $hiddenHandle = $reader.ReadInt32()
                            [void]$hiddenHandles.Add($hiddenHandle)
                        }
                    }
                    "afxCam" {
                        $camPosX = $reader.ReadSingle()
                        $camPosY = $reader.ReadSingle()
                        $camPosZ = $reader.ReadSingle()
                        $camAng0 = $reader.ReadSingle()
                        $camAng1 = $reader.ReadSingle()
                        $camAng2 = $reader.ReadSingle()
                        $camFov = $reader.ReadSingle()
                        if ($summary.mainCameraCount -eq 0) {
                            $summary.firstMainCameraPosX = $camPosX
                            $summary.firstMainCameraPosY = $camPosY
                            $summary.firstMainCameraPosZ = $camPosZ
                            $summary.firstMainCameraAng0 = $camAng0
                            $summary.firstMainCameraAng1 = $camAng1
                            $summary.firstMainCameraAng2 = $camAng2
                            $summary.firstMainCameraFov = $camFov
                        }
                        $summary.lastMainCameraPosX = $camPosX
                        $summary.lastMainCameraPosY = $camPosY
                        $summary.lastMainCameraPosZ = $camPosZ
                        $summary.lastMainCameraAng0 = $camAng0
                        $summary.lastMainCameraAng1 = $camAng1
                        $summary.lastMainCameraAng2 = $camAng2
                        $summary.lastMainCameraFov = $camFov
                        $summary.mainCameraCount++
                    }
                    "deleted" {
                        $deletedHandle = $reader.ReadInt32()
                        [void]$deletedHandles.Add($deletedHandle)
                        $summary.deletedCount++
                    }
                    "entity_state" {
                        $entityHandle = $reader.ReadInt32()
                        [void]$entityHandles.Add($entityHandle)
                        $entityVisible = $false
                        $isViewModel = $false
                        $entityOrigin = $null
                        $entityModelName = $null
                        $hasBones = $false

                        while ($true) {
                            $entityToken = Read-AgrDictionaryToken -Reader $reader -Dictionary $dictionary
                            if ($entityToken -eq "/") {
                                $isViewModel = $reader.ReadBoolean()
                                break
                            }

                            switch ($entityToken) {
                                "baseentity" {
                                    $entityModelName = Read-AgrDictionaryToken -Reader $reader -Dictionary $dictionary
                                    if ($entityModelName) {
                                        [void]$summary.models.Add($entityModelName)
                                    }
                                    $entityVisible = $reader.ReadBoolean()
                                    $matrixValues = New-Object double[] 12
                                    for ($i = 0; $i -lt 12; ++$i) {
                                        $matrixValues[$i] = [double]$reader.ReadSingle()
                                    }
                                    $entityOrigin = @(
                                        $matrixValues[3],
                                        $matrixValues[7],
                                        $matrixValues[11]
                                    )
                                    if ($entityOriginByHandle.ContainsKey($entityHandle)) {
                                        if (Test-AgrVectorChanged -Current $entityOrigin -Reference $entityOriginByHandle[$entityHandle]) {
                                            [void]$movingEntityHandles.Add($entityHandle)
                                        }
                                    }
                                    else {
                                        $entityOriginByHandle[$entityHandle] = $entityOrigin
                                    }
                                }
                                "baseanimating" {
                                    $hasBones = $reader.ReadBoolean()
                                    if ($hasBones) {
                                        $boneCount = $reader.ReadInt32()
                                        $summary.boneEntityCount++
                                        $summary.boneCount += $boneCount
                                        if ($boneCount -gt $summary.maxBoneCount) {
                                            $summary.maxBoneCount = $boneCount
                                        }
                                        for ($boneIndex = 0; $boneIndex -lt $boneCount; ++$boneIndex) {
                                            $boneMatrix = New-Object double[] 12
                                            for ($valueIndex = 0; $valueIndex -lt 12; ++$valueIndex) {
                                                $boneMatrix[$valueIndex] = [double]$reader.ReadSingle()
                                            }
                                            if ($boneIndex -eq 0) {
                                                $boneTranslation = @(
                                                    $boneMatrix[3],
                                                    $boneMatrix[7],
                                                    $boneMatrix[11]
                                                )
                                                if ($firstBoneTranslationByHandle.ContainsKey($entityHandle)) {
                                                    if (Test-AgrVectorChanged -Current $boneTranslation -Reference $firstBoneTranslationByHandle[$entityHandle]) {
                                                        [void]$movingBoneHandles.Add($entityHandle)
                                                        if ($isViewModel) {
                                                            [void]$movingViewModelHandles.Add($entityHandle)
                                                        }
                                                    }
                                                }
                                                else {
                                                    $firstBoneTranslationByHandle[$entityHandle] = $boneTranslation
                                                }
                                            }
                                        }
                                    }
                                }
                                "camera" {
                                    [void]$reader.ReadBoolean()
                                    for ($i = 0; $i -lt 6; ++$i) {
                                        [void]$reader.ReadSingle()
                                    }
                                    $playerCameraFov = [double]$reader.ReadSingle()
                                    if ($summary.playerCameraCount -eq 0) {
                                        $summary.firstPlayerCameraFov = $playerCameraFov
                                        $summary.playerCameraFovMin = $playerCameraFov
                                        $summary.playerCameraFovMax = $playerCameraFov
                                    }
                                    else {
                                        if ($playerCameraFov -lt $summary.playerCameraFovMin) {
                                            $summary.playerCameraFovMin = $playerCameraFov
                                        }
                                        if ($playerCameraFov -gt $summary.playerCameraFovMax) {
                                            $summary.playerCameraFovMax = $playerCameraFov
                                        }
                                    }
                                    $summary.lastPlayerCameraFov = $playerCameraFov
                                    $summary.playerCameraCount++
                                }
                                default {
                                    throw "Unsupported AGR entity token '$entityToken' in '$Path'."
                                }
                            }
                        }

                        $summary.entityCount++
                        if ($entityVisible) {
                            $summary.visibleEntityCount++
                        }
                        else {
                            $summary.invisibleEntityCount++
                        }
                        if ($isViewModel) {
                            $summary.viewModelEntityCount++
                            [void]$viewModelHandles.Add($entityHandle)
                            if ($hasBones) {
                                $summary.viewModelBoneEntityCount++
                            }
                            if ($entityModelName) {
                                [void]$summary.viewModelModels.Add($entityModelName)
                            }
                        }
                        elseif ($entityModelName -and $entityModelName.StartsWith("weapons/models/", [System.StringComparison]::OrdinalIgnoreCase)) {
                            $summary.worldWeaponEntityCount++
                            [void]$worldWeaponHandles.Add($entityHandle)
                            [void]$summary.worldWeaponModels.Add($entityModelName)
                        }

                        if ($isViewModel -and $entityOriginByHandle.ContainsKey($entityHandle) -and $movingEntityHandles.Contains($entityHandle)) {
                            [void]$movingViewModelHandles.Add($entityHandle)
                        }
                    }
                    default {
                        throw "Unsupported AGR top-level token '$token' in '$Path'."
                    }
                }
            }

            $summary.uniqueEntityHandleCount = $entityHandles.Count
            $summary.uniqueDeletedHandleCount = $deletedHandles.Count
            $summary.uniqueHiddenHandleCount = $hiddenHandles.Count
            $summary.movingEntityHandleCount = $movingEntityHandles.Count
            $summary.movingBoneEntityCount = $movingBoneHandles.Count
            $summary.movingViewModelHandleCount = $movingViewModelHandles.Count
            $summary.uniqueWorldWeaponHandleCount = $worldWeaponHandles.Count
            $summary.uniqueViewModelHandleCount = $viewModelHandles.Count
            $summary.worldWeaponModelCount = $summary.worldWeaponModels.Count
            $summary.viewModelModelCount = $summary.viewModelModels.Count
            $summary.models = @($summary.models | Sort-Object)
            $summary.viewModelModels = @($summary.viewModelModels | Sort-Object)
            $summary.worldWeaponModels = @($summary.worldWeaponModels | Sort-Object)
            return [PSCustomObject]$summary
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Get-AgrPatternSourceText {
    param(
        $Summary,

        [string]$PropertyName = "models"
    )

    if ($null -eq $Summary) {
        return ""
    }

    $value = $Summary.$PropertyName
    if ($null -eq $value) {
        return ""
    }

    if ($value -is [System.Array] -or $value -is [System.Collections.IEnumerable]) {
        return (($value | ForEach-Object { [string]$_ }) -join [Environment]::NewLine)
    }

    return [string]$value
}

function Get-AgrComparisonValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArtifactPath,

        [string]$PropertyName,

        [hashtable]$SummaryCache
    )

    if (-not (Test-Path -LiteralPath $ArtifactPath)) {
        return $null
    }

    if ($SummaryCache -and -not $SummaryCache.ContainsKey($ArtifactPath)) {
        $SummaryCache[$ArtifactPath] = Read-AgrFileSummary -Path $ArtifactPath
    }

    $summary = if ($SummaryCache) { $SummaryCache[$ArtifactPath] } else { Read-AgrFileSummary -Path $ArtifactPath }
    if (-not $PropertyName) {
        return $summary
    }

    return $summary.$PropertyName
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
    $artifactResultsByPath = @{}
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

        $artifactResult = [PSCustomObject]@{
            path = $artifactPath
            exists = $exists
            size = $size
            minBytes = $minBytes
            valid = $valid
        }
        $artifactResults += $artifactResult
        $artifactResultsByPath[$artifactPath] = $artifactResult
    }

    $artifactStringResults = @()
    $allArtifactStringsMatched = $true
    foreach ($artifactCheck in @((Get-ScenarioPropertyValue -Scenario $scenario -Name "artifactStringChecks" -Default @()))) {
        $artifactPathValue = Expand-Tokens -Value ([string]$artifactCheck.path) -Tokens $tokens
        $artifactPath = Resolve-ArtifactPath -ArtifactPath $artifactPathValue -OutputDir $outputDir
        $exists = Test-Path -LiteralPath $artifactPath
        $printableStrings = if ($exists) { Get-PrintableStrings -Path $artifactPath } else { @() }
        $joinedStrings = ($printableStrings -join [Environment]::NewLine)

        foreach ($pattern in @((Get-ScenarioPropertyValue -Scenario $artifactCheck -Name "requiredPatterns" -Default @()))) {
            $expandedPattern = Expand-Tokens -Value ([string]$pattern) -Tokens $tokens
            $matched = $exists -and [System.Text.RegularExpressions.Regex]::IsMatch($joinedStrings, $expandedPattern)
            if (-not $matched) {
                $allArtifactStringsMatched = $false
            }

            $artifactStringResults += [PSCustomObject]@{
                path = $artifactPath
                kind = "required"
                pattern = $expandedPattern
                matched = $matched
            }
        }

        foreach ($pattern in @((Get-ScenarioPropertyValue -Scenario $artifactCheck -Name "forbiddenPatterns" -Default @()))) {
            $expandedPattern = Expand-Tokens -Value ([string]$pattern) -Tokens $tokens
            $matched = $exists -and [System.Text.RegularExpressions.Regex]::IsMatch($joinedStrings, $expandedPattern)
            $passed = -not $matched
            if (-not $passed) {
                $allArtifactStringsMatched = $false
            }

            $artifactStringResults += [PSCustomObject]@{
                path = $artifactPath
                kind = "forbidden"
                pattern = $expandedPattern
                matched = $matched
                passed = $passed
            }
        }
    }

    $artifactComparisonResults = @()
    $allArtifactComparisonsMatched = $true
    foreach ($comparison in @((Get-ScenarioPropertyValue -Scenario $scenario -Name "artifactSizeComparisons" -Default @()))) {
        $leftPathValue = Expand-Tokens -Value ([string]$comparison.leftPath) -Tokens $tokens
        $rightPathValue = Expand-Tokens -Value ([string]$comparison.rightPath) -Tokens $tokens
        $leftPath = Resolve-ArtifactPath -ArtifactPath $leftPathValue -OutputDir $outputDir
        $rightPath = Resolve-ArtifactPath -ArtifactPath $rightPathValue -OutputDir $outputDir
        $leftExists = Test-Path -LiteralPath $leftPath
        $rightExists = Test-Path -LiteralPath $rightPath
        $leftSize = if ($leftExists) { (Get-Item -LiteralPath $leftPath).Length } else { 0 }
        $rightSize = if ($rightExists) { (Get-Item -LiteralPath $rightPath).Length } else { 0 }
        $operator = [string](Get-ScenarioPropertyValue -Scenario $comparison -Name "operator" -Default "eq")

        $passed = switch ($operator) {
            "gt" { $leftExists -and $rightExists -and ($leftSize -gt $rightSize) }
            "ge" { $leftExists -and $rightExists -and ($leftSize -ge $rightSize) }
            "lt" { $leftExists -and $rightExists -and ($leftSize -lt $rightSize) }
            "le" { $leftExists -and $rightExists -and ($leftSize -le $rightSize) }
            "ne" { $leftExists -and $rightExists -and ($leftSize -ne $rightSize) }
            default { $leftExists -and $rightExists -and ($leftSize -eq $rightSize) }
        }

        if (-not $passed) {
            $allArtifactComparisonsMatched = $false
        }

        $artifactComparisonResults += [PSCustomObject]@{
            leftPath = $leftPath
            leftSize = $leftSize
            operator = $operator
            rightPath = $rightPath
            rightSize = $rightSize
            passed = $passed
        }
    }

    $artifactAgrResults = @()
    $allArtifactAgrMatched = $true
    $agrSummaryCache = @{}
    foreach ($artifactCheck in @((Get-ScenarioPropertyValue -Scenario $scenario -Name "artifactAgrChecks" -Default @()))) {
        $artifactPathValue = Expand-Tokens -Value ([string]$artifactCheck.path) -Tokens $tokens
        $artifactPath = Resolve-ArtifactPath -ArtifactPath $artifactPathValue -OutputDir $outputDir
        $exists = Test-Path -LiteralPath $artifactPath
        $agrSummary = if ($exists) {
            if (-not $agrSummaryCache.ContainsKey($artifactPath)) {
                $agrSummaryCache[$artifactPath] = Read-AgrFileSummary -Path $artifactPath
            }
            $agrSummaryCache[$artifactPath]
        } else { $null }
        $expectations = Get-ScenarioPropertyValue -Scenario $artifactCheck -Name "expectations" -Default $null

        if ($null -eq $expectations) {
            $artifactAgrResults += [PSCustomObject]@{
                path = $artifactPath
                exists = $exists
                passed = $exists
            }

            if (-not $exists) {
                $allArtifactAgrMatched = $false
            }
            continue
        }

        foreach ($property in $expectations.PSObject.Properties) {
            $propertyName = [string]$property.Name
            $actualValue = if ($agrSummary) { $agrSummary.$propertyName } else { $null }
            $actualNumber = if ($null -ne $actualValue) { [double]$actualValue } else { [double]::NaN }
            $passed = $exists -and $null -ne $actualValue -and (Test-AgrNumericExpectation -Actual $actualNumber -Expected $property.Value -Summary $agrSummary)
            if (-not $passed) {
                $allArtifactAgrMatched = $false
            }

            $artifactAgrResults += [PSCustomObject]@{
                path = $artifactPath
                property = $propertyName
                actual = $actualValue
                expected = $property.Value
                passed = $passed
            }
        }
    }

    $artifactAgrComparisonResults = @()
    $allArtifactAgrComparisonsMatched = $true
    foreach ($comparison in @((Get-ScenarioPropertyValue -Scenario $scenario -Name "artifactAgrComparisons" -Default @()))) {
        $leftPathValue = Expand-Tokens -Value ([string]$comparison.leftPath) -Tokens $tokens
        $rightPathValue = Expand-Tokens -Value ([string]$comparison.rightPath) -Tokens $tokens
        $leftPath = Resolve-ArtifactPath -ArtifactPath $leftPathValue -OutputDir $outputDir
        $rightPath = Resolve-ArtifactPath -ArtifactPath $rightPathValue -OutputDir $outputDir
        $leftProperty = [string](Get-ScenarioPropertyValue -Scenario $comparison -Name "leftProperty" -Default "")
        $rightProperty = [string](Get-ScenarioPropertyValue -Scenario $comparison -Name "rightProperty" -Default "")
        $operator = [string](Get-ScenarioPropertyValue -Scenario $comparison -Name "operator" -Default "eq")

        $leftValue = Get-AgrComparisonValue -ArtifactPath $leftPath -PropertyName $leftProperty -SummaryCache $agrSummaryCache
        $rightValue = Get-AgrComparisonValue -ArtifactPath $rightPath -PropertyName $rightProperty -SummaryCache $agrSummaryCache
        $leftExists = $null -ne $leftValue
        $rightExists = $null -ne $rightValue

        $leftNumber = if ($leftExists) { [double]$leftValue } else { [double]::NaN }
        $rightNumber = if ($rightExists) { [double]$rightValue } else { [double]::NaN }
        $tolerance = [double](Get-ScenarioPropertyValue -Scenario $comparison -Name "tolerance" -Default 0)

        $passed = switch ($operator) {
            "gt" { $leftExists -and $rightExists -and ($leftNumber -gt $rightNumber) }
            "ge" { $leftExists -and $rightExists -and ($leftNumber -ge $rightNumber) }
            "lt" { $leftExists -and $rightExists -and ($leftNumber -lt $rightNumber) }
            "le" { $leftExists -and $rightExists -and ($leftNumber -le $rightNumber) }
            "ne" { $leftExists -and $rightExists -and ($leftNumber -ne $rightNumber) }
            "approx" { $leftExists -and $rightExists -and ([math]::Abs($leftNumber - $rightNumber) -le $tolerance) }
            default { $leftExists -and $rightExists -and ($leftNumber -eq $rightNumber) }
        }

        if (-not $passed) {
            $allArtifactAgrComparisonsMatched = $false
        }

        $artifactAgrComparisonResults += [PSCustomObject]@{
            leftPath = $leftPath
            leftProperty = $leftProperty
            leftValue = $leftValue
            operator = $operator
            rightPath = $rightPath
            rightProperty = $rightProperty
            rightValue = $rightValue
            tolerance = $tolerance
            passed = $passed
        }
    }

    $artifactAgrModelResults = @()
    $allArtifactAgrModelsMatched = $true
    foreach ($artifactCheck in @((Get-ScenarioPropertyValue -Scenario $scenario -Name "artifactAgrModelChecks" -Default @()))) {
        $artifactPathValue = Expand-Tokens -Value ([string]$artifactCheck.path) -Tokens $tokens
        $artifactPath = Resolve-ArtifactPath -ArtifactPath $artifactPathValue -OutputDir $outputDir
        $exists = Test-Path -LiteralPath $artifactPath
        $summary = if ($exists) {
            if (-not $agrSummaryCache.ContainsKey($artifactPath)) {
                $agrSummaryCache[$artifactPath] = Read-AgrFileSummary -Path $artifactPath
            }
            $agrSummaryCache[$artifactPath]
        }
        else { $null }

        $propertyName = [string](Get-ScenarioPropertyValue -Scenario $artifactCheck -Name "property" -Default "models")
        $patternText = Get-AgrPatternSourceText -Summary $summary -PropertyName $propertyName

        foreach ($pattern in @((Get-ScenarioPropertyValue -Scenario $artifactCheck -Name "requiredPatterns" -Default @()))) {
            $expandedPattern = Expand-Tokens -Value ([string]$pattern) -Tokens $tokens
            $matched = $exists -and [System.Text.RegularExpressions.Regex]::IsMatch($patternText, $expandedPattern)
            if (-not $matched) {
                $allArtifactAgrModelsMatched = $false
            }

            $artifactAgrModelResults += [PSCustomObject]@{
                path = $artifactPath
                property = $propertyName
                kind = "required"
                pattern = $expandedPattern
                matched = $matched
            }
        }

        foreach ($pattern in @((Get-ScenarioPropertyValue -Scenario $artifactCheck -Name "forbiddenPatterns" -Default @()))) {
            $expandedPattern = Expand-Tokens -Value ([string]$pattern) -Tokens $tokens
            $matched = $exists -and [System.Text.RegularExpressions.Regex]::IsMatch($patternText, $expandedPattern)
            $passed = -not $matched
            if (-not $passed) {
                $allArtifactAgrModelsMatched = $false
            }

            $artifactAgrModelResults += [PSCustomObject]@{
                path = $artifactPath
                property = $propertyName
                kind = "forbidden"
                pattern = $expandedPattern
                matched = $matched
                passed = $passed
            }
        }
    }

    $success =
        $allPatternsMatched `
        -and $allArtifactsValid `
        -and $allArtifactStringsMatched `
        -and $allArtifactComparisonsMatched `
        -and $allArtifactAgrMatched `
        -and $allArtifactAgrComparisonsMatched `
        -and $allArtifactAgrModelsMatched `
        -and (($null -eq $cs2ExitCode) -or ($cs2ExitCode -eq 0))

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
        artifactStringChecks = $artifactStringResults
        artifactSizeComparisons = $artifactComparisonResults
        artifactAgrChecks = $artifactAgrResults
        artifactAgrComparisons = $artifactAgrComparisonResults
        artifactAgrModelChecks = $artifactAgrModelResults
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
