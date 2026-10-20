# Repository Guidelines

## Project Structure & Module Organization

The Visual Studio solution is `lcu_mouse.sln`; `lcu_mouse.vcxproj` builds the x64 `steam_api64.dll` proxy. `dllmain.cpp` owns DLL startup, fixed-RVA hooks, configuration, and camera detours. `RawInput.*` handles mouse capture, while `MouseCameraState.*` contains testable toggle, synchronization, and orbit state. `Proxy.h`, `steam_api64.asm`, and `steam_api64.def` forward Steam API exports. `minhook/` is vendored hook infrastructure. Unit-style tests live in `tests/`; generated files belong under `x64/` or `lcu_mouse/` and should not be committed.

## Build, Test, and Development Commands

Run commands from a Visual Studio Developer PowerShell:

```powershell
msbuild lcu_mouse.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64
cl /nologo /std:c++17 /EHsc /W4 /WX tests\MouseCameraStateTests.cpp MouseCameraState.cpp /Fe:tests\MouseCameraStateTests.exe
.\tests\MouseCameraStateTests.exe
```

The first command produces `x64\Release\steam_api64.dll`. The next two compile and run the state tests independently with warnings treated as errors.

## Coding Style & Naming Conventions

Use tabs for indentation, Allman braces, and the existing C++ style. Types and public methods use `PascalCase`; local variables and state use `camelCase`. Keep Windows-specific operations in the hook/input layer and deterministic state logic in `MouseCameraState.*`. Document reverse-engineered offsets with evidence, and avoid speculative memory writes. Preserve the Win64 ABI exactly when declaring hook signatures.

## Testing Guidelines

Add focused cases to `tests/MouseCameraStateTests.cpp` for state transitions, stale input, timing gaps, and orbit rebasing. Before submitting camera changes, test normal movement, F8 disable/re-enable, focus loss, loading/fast travel, scripted cameras, and ferry travel to Albatross Island. This project targets executable version `1.0.0.15609011`; verify RVAs before supporting another build.

## Commit & Pull Request Guidelines

History uses short title-style subjects. Prefer concise imperative messages such as `Fix camera state across ferry transitions`. Keep commits focused. Pull requests should explain the observed behavior and root cause, list build/test results, identify any changed offsets or `lcu.ini` keys, and provide exact in-game reproduction and verification steps. Do not commit game executables, saves, personal `.vcxproj.user` settings, or local configuration.
