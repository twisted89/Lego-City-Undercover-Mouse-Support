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
