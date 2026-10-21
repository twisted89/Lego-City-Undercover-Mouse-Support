# Mouse-Camera Work

## Symptoms

The original mod normally controlled the third-person camera, but taking the ferry to Albatross Island made the camera spin continuously. After the forced-camera hook was removed, the spin stopped, but a second transition bug became visible: mouse-camera input remained disabled on Albatross and stayed disabled after returning to the mainland.

## Diagnosis

The investigation treated the cause as unknown and checked the hook code, the game executable, and the running process.

Three independent problems were found:

1. The mod hooked the game's scripted/forced-camera routine at RVA `0x8ED0A0` and wrote forced direction fields on every call. Ferry arrival repeatedly uses that path, so the mod continuously fought the scripted camera and produced the spin.
2. The game-global value at RVA `0x18152A4`, historically treated as `isFirstPerson`, changes to `1` during the ferry transition and remains `1` during ordinary third-person play on Albatross. Treating it as authoritative permanently disabled mouse-camera input.
3. Raw input used a message-only window owned by the camera-update thread. Windows destroys such a window when its owning thread exits. Loading can replace that thread, leaving the mod with a stale window handle and no registered raw-mouse target.

Live inspection confirmed the failure state: the camera hook was running, the mod was enabled and focused, `isFirstPerson` was `1`, resynchronization remained pending, and input acceptance was off. The ferry sets the mode flag before the long camera-update gap, which also explained why the first conservative recovery implementation failed: it discarded the last known third-person state too early. Temporarily arming only the mod's recovery gate changed the state to “override active, resync complete, input accepting,” and mouse control immediately resumed. No game state was patched.

## Fix

The forced-camera detour at `0x8ED0A0` was removed. The mod now hooks only the regular camera update at RVA `0x991500` and lets scripted camera transitions run normally.

`CameraModeGate` handles the unreliable mode flag without writing to game memory. It records a confirmed third-person camera and, when a load-sized update gap follows, permits a local stale-flag override only after the original game update produces a finite, non-zero third-person orbit. A mode report that arrives just before the ferry gap no longer erases that evidence while the geometry is still third-person. A collapsed/invalid orbit revokes the override, preserving genuine first-person behavior.

During resynchronization, the mod:

1. Stops accepting input and clears accumulated deltas.
2. Calls the original camera update first.
3. Reconstructs rotation and distance from the game's resulting camera orbit.
4. Re-enables capture only if focus, enable state, revision, camera mode, and positions are still valid.

`RawInput` now validates the message-window identity, process, and owning thread. `Update(true)` unregisters stale capture, creates a new queue on the current camera thread, and registers the mouse against it. Disabled updates never create a new queue.

The added F8 toggle is configurable as hexadecimal `togglekey` under `[settings]` in `lcu.ini` (default `0x77`). It reacts only to the initial key-down event. Disabling releases synthesized buttons, unregisters capture, and clears deltas; re-enabling uses the same camera resynchronization path to avoid a jump.

## Safety Conditions

- The fix does not modify the game's `isFirstPerson` value.
- A startup gap alone cannot override a first-person camera.
- Recovery requires prior third-person evidence, a camera-update gap, and usable post-update orbit geometry.
- Invalid geometry or a real first-person orbit disables input and clears the override.
- State revisions prevent an enable/focus transition from racing with the camera thread.

## Verification

The x64 Release build succeeds. `MouseCameraStateTests` covers toggle edges, stale input, update gaps, orbit rebasing, ferry ordering, and genuine first-person rejection. `RawInputLifecycleTests` uses real Win32 message windows to verify recovery after the original thread exits and migration to a replacement camera thread.

Final manual validation should cover:

1. Normal mainland mouse-camera control.
2. F8 disable/re-enable without stale motion or a jump.
3. Mainland → ferry → Albatross, followed by mouse movement.
4. Albatross → mainland return travel.
5. A genuine first-person/scripted camera sequence.

The reverse-engineered RVAs apply to executable version `1.0.0.15609011`.

## Investigation Update — 2026-10-20

### Why the previous Raw Input recovery still failed

The first ferry-gate fix was correct, but the installed build still lost mouse input on Albatross. A second live-process inspection separated the camera state from the input transport. At the failure point the camera detour was still running approximately every 15 ms, the mod was enabled and focused, the stale-mode recovery gate was armed, resynchronization had completed, and input acceptance was enabled. However, `RawInput` reported itself registered to an HWND for which `IsWindow` returned false.

That evidence identified a lifetime error rather than another camera-mode error. Camera updates can execute on short-lived worker threads. The previous implementation created a message-only Raw Input window on whichever camera thread called `Update(true)`. Windows owns that HWND through the creating thread, so it destroys the window when that worker exits. Recreating the window on a later camera worker only repeats the failure. This supersedes the earlier recovery approach described above; validating and recreating a camera-thread-owned message window is not a durable solution.

No game memory was changed during diagnosis. A temporary in-process state probe was used only to read the mod state and validate the camera gate.

### Durable Raw Input design

`RawInput` no longer creates a hidden window or pumps a camera-thread message queue. The already-hooked game `WndProc` now supplies the game's persistent HWND through `SetTargetWindow` and forwards `WM_INPUT` to `HandleRawInput`. The message is still passed to the original game WndProc afterward, preserving normal game handling.

Camera workers now only request registration state through `Update(bool)`. Enabling registers mouse input to the persistent game HWND; disabling unregisters it. A destroyed or replaced target is detected before registration, and changing targets first removes the old registration. Consequently, a camera worker can exit without destroying the Raw Input destination.

The F8 behavior and camera-transition safeguards remain unchanged: disabling immediately unregisters capture, releases synthesized mouse-button keys, and clears deltas; re-enabling waits for a valid camera update and rebases the orbit before accepting new input.

### Automated evidence

`RawInputLifecycleTests` now uses a real Win32 Raw Input registration and verifies both important lifetime cases:

1. A persistent target window remains valid and registered after the transient camera worker that initializes capture has exited.
2. Destroying and replacing the target removes stale registration and retargets capture to the replacement HWND.

`MouseCameraStateTests` still covers F8 edge handling, state revisions, input clearing, update gaps, ferry ordering, stale mode recovery, valid orbit rebasing, and genuine first-person rejection. Both test executables pass, and a Release x64 rebuild produces `Release\steam_api64.dll` with intermediates under `Release\obj\`. The only build diagnostic is the pre-existing `Vector3.cpp` double-to-float warning.

### Remaining manual validation

The redesigned DLL still requires an in-game ferry run when a tester is available. Verify mainland mouse movement, F8 disable/re-enable, mainland to Albatross, continued movement after arrival, return to the mainland, and at least one genuine first-person or scripted-camera sequence. If a failure recurs, capture whether the camera moves under keyboard/controller input and whether F8 disable/re-enable changes the symptom; those observations distinguish camera gating from Raw Input delivery.

### Installed artifact

The failing DLL inspected during the final live session had SHA-256 `55CDF534322E939F0EEA09D8E03E40550BD071540D1063F57159300E582E565D`. After the persistent-HWND change, the freshly rebuilt DLL and the copy installed in the game directory both have SHA-256 `6A5CF165507058564C629C15DFD5EDE533712056474C8655204496A889FBEAE5`. The game was not running during replacement, and `steam_api64.pre-codex.dll` remains the untouched original backup.

### Final live validation — 2026-10-20

The installed `6A5CF...AE5` build was tested in the running game while a read-only external state probe watched the mod globals. Mouse-camera control worked on Albatross, continued working after ferry travel back to the mainland, and did not resume the original spin. After each active transition the camera detour was current, resynchronization was clear, input acceptance was enabled, and Raw Input was registered to the same persistent game HWND.

The F8 lifecycle was also validated live. Holding F8 caused one disable transition (revision 10): input acceptance became false, deltas remained clear, and Raw Input unregistered. The held key did not toggle repeatedly. A second F8 key-down caused one enable transition (revision 11), completed camera resynchronization, restored input acceptance, and re-registered Raw Input. Focus loss afterward independently cleared and unregistered input as designed.

This completes the manual ferry-to-Albatross, return-ferry, hotkey-edge, disable cleanup, and re-enable recovery checks for executable version `1.0.0.15609011`.

### Handoff boundary

The post-commit work begins at **Investigation Update — 2026-10-20**. Everything before that heading reflects commit `5168d81`; the appended material records only the subsequent live diagnosis, final Raw Input redesign, build/install, and successful in-game validation.

### Smooth-rotation follow-up — 2026-10-20

The original camera calculation converted the updated floating-point angle to `int` solely to use integer `% 360`. That discarded fractional degrees on every camera update, made opposite directions asymmetric because integer conversion truncates toward zero, and prevented small sensitivity adjustments from accumulating smoothly.

`AdvanceRotation` now performs the complete calculation in floating point and uses `std::fmod` to normalize into `[0, 360)` without losing the fraction. `UpdateCamDetour` calls this tested helper instead of the integer expression. Regression cases cover quarter-degree movement plus wrapping below 0° and above 360°; the tests were observed failing before the helper existed and passing afterward. The full Release x64 build and Raw Input lifecycle suite also pass.

The rebuilt and installed DLL both have SHA-256 `C91F779BC2B9C803BCDD38AB377FE4980D4D40F2EA0269D9FAC11AA578C67041`. Manual feel testing remains pending. The saved `lcu.ini` sensitivity was `3.280027` at installation time, which is still more than three degrees per raw mouse count; use a value such as `0.25` when specifically evaluating sub-degree motion.

### Smooth-rotation live validation — 2026-10-20

The installed `C91F...7041` build was tested in game with `sensitivity=0.160000`. Slow horizontal camera movement was reported as “smooth as butter,” confirming that fractional rotation now survives from raw mouse input through the camera calculation. The earlier `3.280027` value was a historical read of the same INI before the game later saved `0.160000`; no additional sensitivity change is needed.

The production call site is commented to record why `AdvanceRotation` must remain floating-point: the former integer modulo discarded sub-degree movement and made low-speed rotation asymmetric. Automated coverage protects fractional movement and wrapping across both 0° and 360°.
