# Codex Handoff

Updated 2026-10-21.

## Current goal and state

The ferry/loading mouse-camera fix and the low-speed smoothing follow-up are implemented and committed. `main` is at `47f9f3c` (`mouse smoothing`), after `43106dd` (`mouse bug finally fixed`); the working tree was clean before this handoff was added. There is no known active code bug or uncommitted implementation to recover.

If work continues, preserve the validated behavior below. The most useful immediate cleanup is bringing `docs/mousework-human.md` in line with the current Raw Input design; additional engineering should be driven by a reproduced game issue.

## Decisions and constraints

- Target only game executable `1.0.0.15609011`; all RVAs are version-specific.
- Keep the regular camera-update hook at RVA `0x991500`. Do not restore the forced/scripted-camera detour at `0x8ED0A0` or speculative camera-object writes; that caused ferry-arrival spinning.
- The value at RVA `0x18152A4` is only a fallible reported mode, despite the `isFirstPerson` name. Never patch it. `CameraModeGate` may locally override a stale report only after prior third-person evidence, a load-sized update gap, and a usable post-update orbit.
- Raw Input must target the game's persistent HWND supplied by the hooked `WndProc`. `WndProc` forwards `WM_INPUT` to `RawInput::HandleRawInput` and then calls the original game WndProc. Do not recreate the superseded camera-worker-owned message window.
- On focus, toggle, or load transitions: suspend input, clear deltas, let the game update first, rebase rotation/distance from the resulting orbit, then resume only if the desired-state revision and camera conditions still match.
- Keep rotation floating-point end to end. `AdvanceRotation` uses `std::fmod` and normalizes to `[0, 360)`; the former integer `% 360` path discarded fractional degrees.
- F8 is the default mouse-camera toggle (`[mouse] toggle=F8`); F9 is the default window-mode toggle (`[window] toggle=F9`). Toggle bindings are keyboard-only and accept friendly names, single characters, or decimal/hex virtual-key codes.
- Mouse-camera sensitivity defaults to `0.003`; Home/End adjust it by `0.001`. Windowed mode defaults to 80% aspect-preserving scale, with fixed client defaults of `1280x720`.
- Initial key-down toggles once; disabling mouse-camera control must unregister capture, clear deltas, and release synthesized buttons.
- Preserve Win64 hook signatures/ABI, tabs, and Allman braces. Do not add support for another executable build without re-verifying RVAs.

## Relevant files

- `src/game/dllmain.cpp` - hook installation, WndProc forwarding, transition/resync flow, and camera detour.
- `src/camera/MouseCameraState.{h,cpp}` - deterministic desired/input state, gap detection, mode gate, orbit rebasing, and `AdvanceRotation`.
- `src/input/RawInput.{h,cpp}` - persistent target HWND, registration, and `WM_INPUT` processing.
- `src/tests/MouseCameraStateTests.cpp` - toggle, stale input, revision, ferry ordering, first-person rejection, rebase, and fractional rotation tests.
- `src/tests/RawInputLifecycleTests.cpp` - real Win32 registration tests for a persistent/replaced target HWND.
- `src/steam_proxy/` - proxy exports and forwarding assembly/definition files.
- `src/third_party/minhook/` and `src/third_party/mini/` - vendored dependencies.
- `docs/mousework.md` - authoritative investigation and validation record; later dated sections supersede earlier descriptions.
- `docs/mousework-human.md` - useful background, but its Raw Input message-window description and "remaining validation" status are stale.

## Implemented behavior

- Removed the forced-camera hook that fought ferry scripted cameras.
- Added conservative stale-mode recovery and post-transition orbit rebasing without writing game state.
- Added revision-safe F8/focus lifecycle handling and stale-input cleanup.
- Replaced transient camera-thread Raw Input windows with registration on the persistent game window.
- Preserved sub-degree mouse rotation and symmetric wrapping through `AdvanceRotation`.

## Verification evidence

Fresh checks run while creating this handoff:

- `msbuild LegoCitiesUndercoverPlus.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal` - passed after source reorganization; produces `Release/steam_api64.dll`, with intermediates under `Release/obj/`. The existing `src/camera/Vector3.cpp(16)` C4244 warning remains.
- `cl /nologo /std:c++17 /EHsc /W4 /WX src\tests\MouseCameraStateTests.cpp src\camera\MouseCameraState.cpp /Fe:Release\obj\MouseCameraStateTests.exe`, then the executable - passed.
- `cl /nologo /std:c++17 /EHsc /W4 src\tests\RawInputLifecycleTests.cpp src\input\RawInput.cpp Setupapi.lib User32.lib /Fe:Release\obj\RawInputLifecycleTests.exe`, then the executable - passed (`RawInput stable-target tests passed`). The test compile reports the existing C4456 and C4018 warnings at `RawInput.cpp:282` and `RawInput.cpp:301`.
- `lcu_mouse.vcxproj.filters` mirrors the `src/` folder tree in Solution Explorer. `steam_api64.def` and the two standalone test sources are project items but are not compiled into the DLL.

`docs/mousework.md` records successful in-game validation on 2026-10-20 for mainland-to-Albatross ferry travel, return travel, F8 hold/disable/re-enable, focus loss, and smooth low-speed rotation at `sensitivity=0.160000`. The historically installed validated DLL hash is `C91F779BC2B9C803BCDD38AB377FE4980D4D40F2EA0269D9FAC11AA578C67041`; the currently installed game copy was not rechecked during this handoff.

## Unresolved / next steps

1. Update `docs/mousework-human.md` if documentation consistency is requested; its sections 2, 9, 13, 15, and conclusion describe the superseded message-window design or validation as still pending.
2. Optionally clean the two `/W4` warnings in `RawInput.cpp`, then make the Raw Input test compile under `/WX` and document its canonical command.
3. For any camera-gate change, manually retest ferry both ways, F8, focus loss, Criminal Scanner, cutscenes/scripted cameras, and a genuine first-person-like sequence. The narrow residual risk is a special camera with non-zero horizontal orbit plus a load-sized gap being mistaken for stale third-person state.
4. If installing another build, rebuild Release x64, compare source/destination SHA-256 hashes, ensure the game is stopped during replacement, and repeat the relevant in-game checks.
