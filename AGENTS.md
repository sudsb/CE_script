# Notepad3 Agent Instructions

Notepad3 is a Windows-only Win32 C/C++ editor built on Scintilla/Lexilla. Start with [`docs/README.md`](docs/README.md) for the topic index, then read the relevant pages before changing unfamiliar behavior.

## Working in this repository

- Before implementing a feature, inspect the closest existing implementation and its tests. Reuse the established pattern and implement at the common point where relevant call paths converge.
- Make the smallest change that fixes the root cause. Keep unrelated refactoring, formatting, and cleanup out of the diff.
- Add or update focused tests for changed behavior, then run the narrowest relevant test or build. Check existing test failures against current behavior before treating them as regressions.
- For C/C++ changes, read [Codebase and conventions](docs/architecture/codebase-and-conventions.md) for wrapper, buffer-lifetime, undo, and dialog-result constraints.
- Preserve user changes in the worktree. Do not revert unrelated edits.
- If intended behavior is unclear or the change requires unrelated refactoring, dependency changes, or altering a vendored patch, stop and ask with a recommended approach.
- Done means the requested behavior is addressed, focused checks pass, and the diff contains no unrelated changes. State baseline failures, unrun checks, and remaining uncertainty rather than claiming success.

## Exact developer commands

### Build
```powershell
# Discover MSBuild first
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild `
    -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1

# Full linked build (Release x64)
& $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal

# Compiler-only feedback (ClCompile - not a linked build)
& $msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal

# Build all platforms: Build\BuildAll.cmd
# Clean outputs: Build\Clean.cmd
```

### Test (requires Visual C++ build tools + AutoHotkey v2 for GUI)
```powershell
# Encoding detection regression (compiles detector harness)
pwsh -NoProfile -File test\test_files\encoding\TestEncodingDetection.ps1

# Open-directory logic harness (Dialogs.c functions)
pwsh -NoProfile -File test\TestOpenDialogDirectory.ps1

# Create-directory logic harness (PathLib.c / Config.cpp)
pwsh -NoProfile -File test\TestCreateDirectory.ps1

# Docs checker
pwsh -NoProfile -File Build\scripts\Test-Docs.ps1

# Version check against today's date / Versions\build.txt
cmd /d /c "call test\TestFileVersion.cmd"

# GUI smoke test (needs desktop + AutoHotkey v2)
cmd /d /c "cd /d test && call TestAhkNotepad3.cmd"
```

### Formatting / Analysis
- Sources of truth: `src\.clang-format` and `.editorconfig` (UTF-8/CRLF, 4-space, Stroustrup braces)
- clang-format/tidy may live under `VC\Tools\Llvm\x64\bin` even if not on PATH
- No repo-wide clang-tidy or sanitizer recipe established — report as unrun

### Python scripting
```powershell
# Activate git-ignored venv if present
. .\.venv\Scripts\Activate.ps1
python --version
```

## Architecture facts that change how you work

| Area | Key constraint |
|------|----------------|
| **Scintilla calls** | Always use `SciCall.h` wrappers; add missing wrapper there, never raw `SendMessage` |
| **Undo grouping** | Wrap bulk edits with `_BEGIN_UNDO_ACTION_` / `_END_UNDO_ACTION_` (Notepad3.h) |
| **Buffer lifetime** | Never use pointer from `Path_WriteAccessBuf()`/`StrgWriteAccessBuf()` after reallocating ops (`Path_CanonicalizeEx`, `Path_Swap`, `StrgCat`, etc.) — reacquire with size 0 |
| **InfoBox results** | Returns `MAKELONG(button, mode)`; never compare raw to `IDNO` — use `IsYesOkay()`, `INFOBOX_ANSW(r)`, `INFOBOX_MODE(r)` |
| **Globals** | Use existing `Globals`, `Settings`, `Settings2`, `Flags`, `Paths` structs; do not add new globals |
| **String types** | Use `DocPos`/`DocPosU`/`DocLn` for positions, `cpi_enc_t` for encodings, `HSTRINGW`/`HPATHL` not raw buffers |
| **Vendored libs** | Scintilla/Lexilla under `scintilla\`/`lexilla\` with NP3 patches in `np3_patches\`; read `version.txt` for actual versions |

## Test & CI structure

- **Product CI** (`.github/workflows/build.yml`): `windows-2025-vs2026`, Release on Win32, x64, x64_AVX2, ARM64. Version + GUI tests on Win32/x64 only.
- **Native regression CI** (`.github/workflows/native-tests.yml`): runs 3 extracted harnesses (encoding, open-dir, create-dir) on relevant source/test/fixture/workflow changes. No GUI/full-product coverage.
- **Docs CI** (`.github/workflows/docs.yml`): runs docs checker independently, triggers on Markdown-only changes.
- Test binaries built to `Bin\Release_x64_v145\` (platform/config variant in path).
- In CI test step, preserve `|| exit /b 1` after version test so GUI command cannot mask failure.

## Documentation

- Docs may be stale. Verify source paths/symbols before relying on a claim.
- Propose doc changes only for durable invariants, rationale, or traps preventing repeated mistakes. Include exact source/test anchors and verification evidence.
- Keep uncertain observations in [`docs/open-questions.md`](docs/open-questions.md) with what would verify them.
- After editing docs, run `pwsh -NoProfile -File Build\scripts\Test-Docs.ps1`.
- `@RaiKoHoff` is the human documentation approver; no CODEOWNERS routing configured.

## Scoped guidance files

- `src\AGENTS.md` — source-area routing table (File I/O, PathLib, Encoding, Scintilla API, Lexers, Config)
- `src\Config\AGENTS.md` — INI persistence specifics + `Build\Notepad3.ini` reference
- `docs/README.md` — full topic index with 17 domain/architecture pages

## Windows platform support

Supported: Win32 (x86), x64, x64_AVX2, ARM64. ARM 32-bit unsupported (`Release|ARM` maps to Win32).
- Both ARM64 and x64 define `_WIN64`; use `_M_ARM64` or `NP3_BUILD_ARM64` in `TypeDefs.h` to distinguish.
- ARM64 settings: `CETCompat=false`, `TargetMachine=MachineARM64`.

## Configuration reference

User-visible settings: `readme/config/Configuration.md` and `Build\Notepad3.ini`. Press **Ctrl+F7** to open INI in editor. Most `[Settings2]` changes require restart.

## GrepWin integration (external)

Pre-built in `grepWin\portables\`; runtime lookup order:
1. `Settings2.GrepWinPath`
2. `<ModuleDir>\grepWin\grepWin-x{64,86}_portable.exe`
3. `%APPDATA%\Rizonesoft\Notepad3\grepWin\`

Locale mapping in `MuiLanguage.c` → `grepwin.ini` before launch.
