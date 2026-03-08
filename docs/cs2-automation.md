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
- `artifactStringChecks`: per-artifact regex checks against printable strings extracted from the binary payload.
- `artifactSizeComparisons`: simple artifact size comparisons like `gt`, `ge`, `lt`, `le`, `eq`, `ne`.
- `artifactAgrChecks`: per-artifact AGR payload checks against parsed counters such as `entityCount`, `mainCameraCount`, `playerCameraCount`, `viewModelEntityCount`, `invisibleEntityCount`, `boneEntityCount`, `boneCount`, `maxBoneCount`, `movingEntityHandleCount`, and `movingBoneEntityCount`.
- `artifactAgrComparisons`: cross-artifact comparisons against parsed AGR summary properties.
- Numeric AGR expectations also support `{ "operator": "approx", "value": <number>, "tolerance": <number> }` for camera / transform checks that need a small floating-point tolerance.

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
- `tools/cs2-automation/scenarios/shinden-agr-smoke.json` is the current AGR smoke test. On the verified run from March 8, 2026, it passed with `mirv_agr enabled 1`, `start`, `stop`, console success markers, and a produced `.agr` artifact.
- `tools/cs2-automation/scenarios/shinden-agr-frame-parity.json` is the section 2 frame-writer regression. On the verified run from March 8, 2026, it passed and proved `mainCameraCount == frameCount`, exact first-sample `afxCam` position/angles/FOV from a scripted `mirv_input` camera, `afxHidden` emission after a mid-recording toggle change, and `deleted` records after a same-recording `demo_gototick` jump.
- `tools/cs2-automation/scenarios/shinden-agr-entity-parity.json` is the section 3 entity-data regression. On the verified run from March 8, 2026, it passed and proved player exports carry bones, visible-world weapons stay separate from hidden carried inventory, projectiles classify independently, `recordViewModels -1` still exports viewmodel pieces with `recordInvisible 0`, and `recordInvisible 1` increases weapon handle coverage without reintroducing player models.
- `tools/cs2-automation/scenarios/shinden-agr-animation-fidelity.json` is the section 4 animation / transform regression. On the verified run from March 8, 2026, it passed and proved all three spectator cases (`spec_mode 4` POV, `spec_mode 5` observer-target chase, and `spec_mode 6` HLTV/freecam) still produce valid AGR output, that POV / observer captures keep non-trivial viewmodel skeleton export (`maxBoneCount > 1`), and that parsed recordings show moving entity transforms instead of only static frame snapshots.
- CS2 AGR currently writes binary format version `6`, matching the existing Source 1 AGR importer path. Do not bump that version until the on-disk payload changes and the importer/docs are updated together.
- `artifactAgrChecks` use a lightweight AGR parser built into the wrapper. Its `entityCount` is the number of `entity_state` records across all frames, not the number of unique entity handles.
- The AGR parser also exposes unique-handle counts, first / last main-camera samples, maximum bone count per record, and counts of entity handles whose root or first-bone translations changed over the clip. That lets scenarios validate stable handle reuse, exact `afxCam` payload values, and basic motion fidelity instead of only checking for file existence.
- `artifactAgrChecks` can compare one parsed property against another by using `{ "operator": "eq", "valueFromProperty": "entityCount" }`. The current toggle-matrix scenario uses that to assert that `viewmodels-only.agr` contains only viewmodel-flagged entity records.
- When comparing multi-frame viewmodel recordings like `recordViewModels -1` versus legacy `recordViewModel 1`, compare `uniqueEntityHandleCount` rather than raw `entityCount` or `viewModelEntityCount`. The frame counts can differ while the exported owner set is still the same.
- Prefer parsed AGR checks over raw byte-size comparisons when recordings happen in separate tick windows. File sizes can drift with rendered-frame count even when the toggle behavior is still correct.
- The wrapper exits non-zero if the launch fails, if required log markers are missing, or if expected artifacts are not produced.
