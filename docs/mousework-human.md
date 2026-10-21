# LEGO City Undercover Mouse Mod — Human Guide

This is the human-oriented companion to `mousework.md`. It keeps the important technical details, but tells the story of the bug and fix in a way that is easier to follow.

## What was broken

The original mouse-camera mod worked normally until ferry travel to Albatross Island.

The first symptom was a continuously spinning camera after arrival. Once that was fixed, a second problem became visible: the camera stopped spinning, but mouse-camera control remained dead on Albatross and could stay dead after returning to the mainland.

These turned out to be separate failures triggered by the same transition.

## Problem 1: the mod was fighting the game's scripted camera

The original mod hooked a scripted/forced-camera routine at RVA `0x8ED0A0` and changed camera fields every time the game used that routine.

That was harmless during ordinary play, but the ferry transition uses scripted camera behavior heavily. The game and the mod were effectively trying to control the same camera at the same time, which produced the spin.

The fix was simple: remove that forced-camera hook and leave scripted camera transitions alone. The mod now hooks only the normal camera update at RVA `0x991500`.

That eliminated the spinning.

## Problem 2: the supposed `isFirstPerson` flag is unreliable

The mod also watches a game value at RVA `0x18152A4`, historically treated as `isFirstPerson`.

Live inspection showed that this value changes to `1` during the ferry transition and can remain `1` even after the player is clearly back in normal third-person play on Albatross.

So the old logic:

```text
game says first person
→ disable mouse camera
```

was too trusting.

The final camera logic now combines two signals:

- what the game reports;
- whether the actual camera and player positions still form a valid third-person orbit.

If the game suddenly reports first person but the camera still looks like a normal third-person camera, the mod blocks input temporarily but preserves the fact that third-person play was previously confirmed.

After a loading-sized pause, it lets the game establish the new camera first, checks the resulting geometry, and only then decides whether the first-person flag is stale.

The important safety point is that the mod does not patch the game's flag. It only decides whether to trust it.

## Why the first recovery attempt still failed

The ferry changes the mode flag before the long loading pause:

```text
normal third-person play
        ↓
game flag changes to "first person"
        ↓
camera is still geometrically third-person
        ↓
camera updates stop for loading
        ↓
Albatross loads
```

The first recovery implementation erased its memory of third-person play as soon as the flag changed. By the time the actual load gap occurred, it had thrown away the evidence needed to recognize the ferry case.

The corrected `CameraModeGate` keeps that evidence as long as the camera geometry still looks third-person.

## Problem 3: Raw Input was attached to a short-lived thread

This was the deeper input bug.

The original Raw Input code created a hidden message-only window on whichever camera-update thread happened to call it. Windows windows are owned by the thread that created them.

During loading, LEGO City Undercover can replace that camera worker thread. When the old thread exits, Windows destroys its hidden window.

That left the mod with something like:

```text
camera hook: running
mod enabled: yes
input accepted: yes
Raw Input target HWND: dead
```

The camera code was healthy, but Windows had nowhere valid to deliver raw mouse messages.

An earlier attempt tried to detect the dead window and recreate it on the next camera thread. Live testing showed why that was not durable: the replacement camera thread could also be short-lived, so the same failure simply happened again.

## The final Raw Input design

The final solution stops using a camera-thread-owned hidden window entirely.

The mod already hooks the game's main `WndProc`, so it now uses the game's persistent window as the Raw Input target.

The flow is now:

```text
game HWND
   ↓
WM_INPUT
   ↓
hooked game WndProc
   ↓
RawInput::HandleRawInput(...)
   ↓
mouse-camera state
```

Camera worker threads no longer own or pump an input window. They only request whether Raw Input should be registered or unregistered.

This is much more durable because ferry/loading transitions can replace camera workers without destroying the Raw Input destination.

The game's original `WndProc` still receives the message afterward, so normal game handling is preserved.

## Camera resynchronization after transitions

The mod also avoids carrying stale camera state across loads.

When a transition is detected, it:

1. stops accepting mouse input;
2. clears accumulated mouse deltas;
3. lets the original game camera update run first;
4. reads the camera position chosen by the game;
5. reconstructs the mod's rotation and distance from that position;
6. resumes capture only if the game state is still valid.

This means the mod adapts to the game's new camera instead of snapping the game back to an old pre-load orbit.

## F8 toggle

F8 now enables or disables mouse-camera control.

The default is:

```ini
[settings]
togglekey=0x77
```

`0x77` is F8.

Disabling does more than flip a boolean: it unregisters Raw Input, clears pending movement, and releases any synthesized mouse-button key presses.

Re-enabling uses the same camera resynchronization path, so stale deltas or an old orbit should not cause a jump.

Holding F8 does not repeatedly toggle because only the initial key-down event is accepted.

## What was actually validated in the game

The final Raw Input redesign was tested live, not just in unit tests.

The rebuilt DLL was verified on executable version `1.0.0.15609011` and successfully handled:

- normal mouse-camera movement on Albatross;
- ferry travel back to the mainland;
- no return of the original spinning camera;
- Raw Input staying registered to the same persistent game HWND;
- F8 disable;
- F8 re-enable;
- held-F8 key-repeat protection;
- focus loss correctly disabling and unregistering input.

During those transitions, the camera detour remained active, resynchronization completed, input acceptance returned, and Raw Input remained attached to the persistent game window.

That is much stronger evidence than the earlier source-level theory alone.

## Smooth-rotation follow-up

A separate camera-quality issue was also found afterward.

The original rotation calculation converted the floating-point angle to an integer solely so it could use `% 360`.

That meant fractional movement was discarded on every update.

At low sensitivity, tiny mouse movements might never accumulate properly, and left/right movement could behave slightly differently because integer conversion truncates toward zero.

The code now keeps the calculation in floating point and normalizes with `std::fmod`.

Conceptually:

```text
old:
fractional angle
→ convert to int
→ lose fraction
→ wrap

new:
fractional angle
→ keep fraction
→ floating-point wrap
```

This was tested in-game at `sensitivity=0.160000`, where slow camera movement was reported as “smooth as butter.”

Tests also cover fractional movement and wrapping across both 0° and 360°.

## Where to look in the code

For someone new to the project, the easiest reading order is:

1. `MouseCameraState.h` — names the important state and concepts.
2. `MouseCameraState.cpp` — contains the camera-mode gate, transition logic, orbit rebasing, and smooth rotation helper.
3. `dllmain.cpp` — connects that state logic to the actual game hooks.
4. `RawInput.cpp/.h` — handles registration and delivery of Windows Raw Input through the game's HWND.
5. `tests/` — shows the behaviors the code is expected to preserve.

The most important concepts are:

- `CameraModeGate` — decides whether the reported first-person state is trustworthy;
- `RebaseOrbit` — derives the mod's orbit from the camera chosen by the game after a transition;
- `AdvanceRotation` — preserves fractional mouse movement;
- the hooked game `WndProc` — now serves as the stable Raw Input delivery point.

## What future changes should preserve

A future contributor should be careful not to undo the lessons from this bug:

- do not restore the old forced-camera hook without strong evidence;
- do not assume `isFirstPerson` is authoritative across loads;
- do not patch the game's mode flag merely to satisfy the mod;
- do not attach Raw Input lifetime to a transient camera worker thread;
- clear stale deltas when disabling or transitioning;
- let the game establish its post-load camera before rebasing;
- keep rotation calculations in floating point;
- remember that all hard-coded RVAs are specific to game version `1.0.0.15609011`.

## Bottom line

The ferry bug was not one bug.

It was a chain of three interacting problems:

1. the mod fought a scripted camera and caused spinning;
2. a stale game mode flag prevented mouse-camera recovery;
3. Raw Input was attached to a window owned by a short-lived camera thread.

The final design addresses each one separately.

After that, the rotation math was also cleaned up so low-sensitivity movement retains fractional precision.

The result is a much more robust mod: scripted transitions are left alone, camera state is re-established after loads, Raw Input is tied to the persistent game window instead of worker-thread lifetime, and slow mouse movement remains smooth.
