# CS2 Automation

`tools/cs2-automation/Run-Cs2Automation.ps1` is a non-interactive test harness for CS2 demo playback through HLAE.

It does four things for each run:

1. Creates a fresh moviemaking config root under `build/cs2-automation/<scenario>-<timestamp>/`.
2. Writes a `mirv_cmd` XML schedule and a per-run `autoexec.cfg` snapshot from a JSON scenario.
3. Launches CS2 through `HLAE.exe -cs2Launcher -autoStart -noGui` with `-netconport` and `-afxFixNetCon`.
4. Moves the CS2 window to a small window on the requested screen, restores focus to the previous window, drives commands through netcon, captures the transcript, and validates required log markers plus artifact files.

## Prerequisites

- Build HLAE and install/package output so either `build\Release\dist\bin\HLAE.exe` or `build\Release\advancedfx-Win32-install\bin\HLAE.exe` exists.
- Close any already running `cs2.exe` process before starting a run.
- Put demos where `playdemo` can load them. The provided scenario uses a demo inside `game\csgo`.

## Scenario format

Required fields:

- `name`: run name prefix.
- `cs2Exe`: full path to `cs2.exe`.
- `demoPath`: full path to the `.dem` file.

Useful optional fields:

- `screenIndex`: zero-based screen index. Default is `1`.
- `windowWidth` / `windowHeight`: default is `854x480`.
- `netConPort`: TCP port for CS2 netcon. Default is `2121`.
- `bootstrapCommands`: commands executed before `playdemo`.
- `tickCommands`: array of `{ "tick": <number>, "command": "<console command>" }`.
- `requiredLogPatterns`: regex patterns that must appear in the captured console log.
- `expectedArtifacts`: files that must exist after the run.

Available string tokens inside commands, patterns, and artifact paths:

- `{{scenarioName}}`
- `{{outputDir}}`
- `{{outputDirForward}}`
- `{{artifactsDir}}`
- `{{artifactsDirForward}}`
- `{{consoleLogPath}}`
- `{{consoleLogPathForward}}`
- `{{commandSystemPath}}`
- `{{commandSystemPathForward}}`
- `{{bootstrapCfgPath}}`
- `{{bootstrapCfgPathForward}}`
- `{{demoPath}}`
- `{{demoPathForward}}`

## Usage

From Windows PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File tools\cs2-automation\Run-Cs2Automation.ps1 `
  -ScenarioPath tools\cs2-automation\scenarios\shinden-demo-smoke.json
```

From WSL:

```bash
/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe \
  -ExecutionPolicy Bypass \
  -File "$(wslpath -w tools/cs2-automation/Run-Cs2Automation.ps1)" \
  -ScenarioPath "$(wslpath -w tools/cs2-automation/scenarios/shinden-demo-smoke.json)"
```

## Outputs

Each run writes these files under the generated output directory:

- `mmcfg/cfg/autoexec.cfg`: generated snapshot of bootstrap commands for the run.
- `command-system.xml`: generated `mirv_cmd` schedule.
- `logs/cs2-console.log`: captured netcon transcript.
- `logs/automation.log`: wrapper log.
- `result.json`: machine-readable pass/fail summary for future AI-driven checks.

## Notes

- Window placement and focus restoration are best-effort. The script moves the game after the CS2 main window appears and then restores focus to the previously active window.
- The wrapper waits for `mirv_cmd` to become available over netcon before it loads HLAE tick commands. This avoids racing HLAE command registration during startup.
- `tools/cs2-automation/scenarios/shinden-demo-smoke.json` is the verified passing smoke test for the provided demo.
- `tools/cs2-automation/scenarios/shinden-agr-smoke.json` is an AGR-oriented template. On the current verified run, CS2 reported `Unknown command: mirv_agr`, so that scenario is expected to fail until AGR is available in the hooked build again.
- The wrapper exits non-zero if the launch fails, if required log markers are missing, or if expected artifacts are not produced.
