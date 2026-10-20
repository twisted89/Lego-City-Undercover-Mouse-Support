# Mouse-Camera Bug: Human Guide

This document is the human-oriented companion to `docs/mousework.md`.

`mousework.md` is useful as a compact technical record for coding agents and future implementation work. This file explains the same bug and fix more narratively: what the player saw, what the mod was doing, why the failure was confusing, how the diagnosis evolved, and what the current fix is trying to guarantee.

---

## 1. What the player actually experienced

The original mouse-camera mod works normally through most of *LEGO City Undercover*. The failure appeared when traveling by ferry to Albatross Island.

At first, the symptom was dramatic:

- the ferry transition completed;
- the player arrived on Albatross Island;
- the camera began spinning continuously.

Removing one suspicious camera hook stopped the spinning. That revealed a second problem:

- the camera no longer spun;
- but mouse-camera control stayed disabled after arrival;
- returning to the mainland did not necessarily restore it.

Those two symptoms turned out to have different causes.

That distinction matters. “Camera spin” and “mouse no longer works” looked like one ferry bug from the player's point of view, but the mod was actually failing in several independent ways during the same loading transition.

---

## 2. What the original mod does

The mod is a native C++ DLL injected into the game as a Steam API proxy.

Its job is roughly:

1. receive raw mouse movement from Windows;
2. hook the game's camera-update function;
3. turn horizontal mouse movement into an orbit angle;
4. modify the camera position before the game finishes its update.

The important pieces are:

- `dllmain.cpp` — installs hooks and coordinates game state, camera state, and input;
- `RawInput.cpp/.h` — receives raw mouse input through a hidden Windows message-only window;
- `MouseCameraState.cpp/.h` — new code that isolates the mouse-camera state machine, transition handling, and orbit rebasing;
- `tests/` — focused tests for state transitions and Raw Input lifecycle behavior.

The mod necessarily depends on reverse-engineered game addresses. The current work targets executable version `1.0.0.15609011`.

---

## 3. The first problem: the mod was fighting a scripted camera

The original mod hooked two camera-related game functions.

The regular third-person camera update at:

```text
RVA 0x991500
```

was expected.

The more suspicious hook was installed at:

```text
RVA 0x8ED0A0
```

and the detour forcibly wrote values inside a game camera object whenever the game tried to force a camera direction.

Conceptually, that meant:

```text
Game: "For this scripted sequence, point the camera this way."
Mod:  "No. Replace part of that camera state."
Game: "Point it this way."
Mod:  "No."
Game: "Point it this way."
Mod:  "No."
```

The ferry arrival makes heavy use of scripted camera behavior. The mod and game therefore kept fighting over the camera state, producing the continuous spin.

### The fix

That forced-camera detour was removed.

The mod now hooks only the normal camera update and allows scripted camera transitions to run without those unexplained writes into the game's camera object.

This was the change that stopped the spinning.

---

## 4. The second problem: `isFirstPerson` is not trustworthy during the ferry transition

The mod has historically treated the value at:

```text
RVA 0x18152A4
```

as an `isFirstPerson` flag.

That name is useful shorthand, but it is reverse-engineered game state, not an official API contract.

Live inspection showed something important:

1. On the mainland, ordinary third-person play behaves as expected.
2. As the ferry transition begins, the value changes to `1`.
3. Camera updates then pause for loading.
4. After arrival on Albatross Island, the value can remain `1`.
5. Visually, however, the game is clearly back in an ordinary third-person camera.

The old logic effectively said:

```text
if game says first-person:
    disable mouse camera
```

That is conservative, but on Albatross it becomes permanently wrong.

This explains the second symptom: once the spin was fixed, the mod still refused to resume mouse control because it believed the stale game flag.

---

## 5. Why the first attempted recovery still failed

The first recovery design was intentionally cautious.

It tried to remember whether the mod had previously seen a valid third-person camera, then use a long camera-update gap as evidence that a loading transition had occurred.

That was reasonable, but the ferry has an awkward event ordering:

```text
Normal third-person play
        ↓
game's "first-person" value flips to 1
        ↓
camera is STILL geometrically third-person
        ↓
camera updates pause for ferry/loading
        ↓
Albatross loads
        ↓
the game value is still 1
```

The first implementation erased its memory of “we were in third person” as soon as the game value changed to `1`.

By the time the actual loading gap occurred, the mod had thrown away the evidence it needed to recognize the stale-flag case.

That is the subtle bug fixed by the latest commit.

---

## 6. The key insight: trust camera geometry as an independent signal

The fix no longer treats the game flag as sufficient evidence by itself.

The mod also asks:

> Do the camera and player positions still describe a usable third-person orbit?

`HasUsableOrbit()` checks the geometry independently of the game mode flag.

This gives the mod two kinds of evidence:

- **reported mode** — what the reverse-engineered game value says;
- **actual geometry** — whether the camera is physically separated from the player in a finite, non-zero orbit.

So when the ferry raises the mode flag *before* the loading gap, the current code behaves like this:

```text
Game says "first person"
        +
Camera still looks like a valid third-person orbit
        ↓
Block mouse input for now,
but KEEP the memory that we had a valid third-person camera.
```

Then the camera-update gap occurs.

After loading, the mod lets the original game camera update run first. If the resulting camera geometry is again a usable third-person orbit, the mod can conclude:

> The mode flag is stale. It is safe to ignore it locally and resume mouse-camera control.

Importantly, the mod does **not** change the game's flag in memory. It only changes its own interpretation of it.

---

## 7. `CameraModeGate`: what it is protecting

The new `CameraModeGate` class exists to make that decision explicit and testable.

It tracks three ideas:

### `thirdPersonObserved`

The mod recently observed evidence consistent with normal third-person play.

During the ferry's early mode-flag change, this evidence is now preserved as long as the camera geometry is still usable.

### `recoveryPending`

A long camera-update gap occurred after known third-person play, so a load/fast-travel recovery may be needed.

A gap by itself is not enough. Startup also contains a gap, and genuine first-person sequences can contain pauses.

### `staleFirstPersonOverride`

The mod has enough evidence to conclude that the game-reported first-person state is stale for the current third-person camera.

This is only a **local override inside the mod**. The game-owned value is left untouched.

If camera geometry later becomes invalid or collapses in a way consistent with genuine first-person behavior, the override is revoked.

---

## 8. Camera resynchronization after a load

Another important part of the fix is that the mod no longer assumes its old orbit state is still correct after a transition.

Before ferry travel, the mod has its own:

- `rotation`;
- `distance`;
- accumulated mouse deltas.

During a scripted transition, the game may move the camera independently. If the mod immediately resumes using its old values, the camera can jump or fight the game.

The resynchronization path therefore does this:

1. stop accepting mouse-camera input;
2. clear accumulated mouse deltas;
3. call the game's original camera update first;
4. observe the camera position the game chose;
5. reconstruct the mod's rotation and distance from that resulting orbit;
6. resume capture only if all relevant state is still valid.

This is what `RebaseOrbit()` is for.

Rather than forcing the post-load camera back to where the mod *used* to think it belonged, the mod asks:

> Where did the game put the camera? Good. That is now our starting orbit.

That makes transitions much less disruptive.

---

## 9. The third problem: Raw Input is tied to a Windows thread

The ferry exposed another issue unrelated to the camera mathematics.

Windows Raw Input is received through a hidden **message-only window**.

That window belongs to the thread that created it.

The original implementation effectively assumed that the camera-update thread would remain the same indefinitely. Loading can violate that assumption.

If the thread owning the message window exits:

- Windows destroys that window;
- the stored `HWND` becomes stale;
- the mod may no longer have a valid destination for raw mouse input.

So the camera hook can still be running correctly while the mouse appears dead.

### The fix

`RawInput` now validates:

- that the stored handle is still a real window;
- that it belongs to this process;
- that it is the mod's own Raw Input window;
- that the current camera thread actually owns it.

If not, the mod unregisters stale capture and creates a new message-only window on the current camera thread.

The code also tags the hidden window with the `RawInput` object pointer through `GWLP_USERDATA`. This prevents a recycled Windows handle from accidentally being mistaken for the mod's old window.

This is subtle Windows plumbing, but it matters because the symptom is otherwise indistinguishable from a broken camera state machine: the mouse simply stops affecting the game.

---

## 10. Why the latest change is convincing

The latest commit is relatively small compared with the earlier refactor.

The central behavioral change is essentially:

```cpp
if (!staleFirstPersonOverride)
{
    if (!orbitUsable)
    {
        thirdPersonObserved = false;
    }
    return false;
}
```

Previously, the mod always cleared `thirdPersonObserved` as soon as the game reported first person.

Now it clears that evidence only when the geometry also stops looking like a usable third-person orbit.

That matches the observed ferry ordering.

The diagnosis is also stronger than a source-code guess because live inspection reportedly confirmed the failure state:

- the camera hook was still executing;
- the mod was enabled;
- the game window was focused;
- the game-reported first-person value was `1`;
- camera resynchronization remained pending;
- mouse input acceptance was off.

Temporarily arming only the mod's recovery gate moved the system into:

```text
override active
resync complete
input accepting
```

and mouse control immediately resumed.

No game memory had to be patched to make that happen.

That is good evidence that the stale mode flag and the recovery state machine were central to the remaining failure.

---

## 11. The F8 enable/disable toggle

The mod also now has an explicit mouse-camera enable/disable hotkey.

Default:

```ini
[settings]
togglekey=0x77
```

`0x77` is F8.

The toggle is designed as more than a simple boolean around one line of camera code.

When disabling, the mod:

- stops accepting new mouse-camera deltas;
- clears accumulated deltas;
- unregisters Raw Input capture;
- releases synthesized mouse-button keyboard actions if necessary.

When re-enabling, it goes through the camera-resynchronization path rather than immediately applying stale camera state.

The toggle also ignores Windows key-repeat, so holding F8 does not rapidly toggle the mod on and off.

---

## 12. Why the state revision exists

Enable/disable and focus changes can occur in the game's window-message thread while camera work may occur elsewhere.

That means state can change while a camera update is in progress.

`DesiredState` therefore publishes:

- enabled state;
- focus state;
- a monotonically changing revision.

The camera path can check that the state revision it started with is still current before accepting input or completing a transition.

This is defensive synchronization: it reduces the chance that a focus loss, F8 press, or re-enable event races with the camera thread and leaves the mod half-enabled.

---

## 13. The tests and what they actually prove

### `MouseCameraStateTests`

These tests cover logic that can be exercised without running the game:

- F8 only toggles on the initial key-down;
- held-key auto-repeat is ignored;
- enable/focus revisions behave correctly;
- stale mouse deltas are cleared;
- update gaps are detected;
- orbit rotation/distance can be reconstructed;
- a startup gap cannot automatically authorize a first-person override;
- the ferry ordering is preserved:
  - first-person report arrives,
  - third-person geometry remains valid,
  - loading gap follows,
  - recovery is then permitted;
- a modeled genuine first-person case is rejected.

### `RawInputLifecycleTests`

These use real Win32 message windows to test the OS-level plumbing:

- the original Raw Input window disappears when its creating thread exits;
- `RawInput` can recover by creating a new one;
- the queue can migrate to the current camera thread;
- mouse registration targets the recovered window.

These tests are valuable because the Raw Input problem is partly about Windows lifecycle behavior, not just pure C++ logic.

---

## 14. What the tests do **not** prove

The final authority is still the real game.

The biggest remaining assumption is the definition of “usable third-person orbit.”

At present, the geometry check is intentionally simple: the camera/player positions must be finite and have non-zero horizontal separation.

That works for the observed ferry failure, but an unusual scripted or first-person camera could theoretically also have non-zero separation.

The dangerous pattern would be:

```text
genuine special/first-person camera
        +
non-zero camera/player horizontal separation
        +
long (>500 ms) camera-update gap
```

If such a sequence exists, the recovery gate could potentially classify it as the stale ferry condition.

That does not mean the fix is wrong. It means the remaining risk is now narrow and testable.

---

## 15. Manual validation that matters most

Before considering this finished, test these in the actual game:

1. **Normal mainland play**
   - mouse rotates the camera normally;
   - scroll distance still behaves normally;
   - mouse-button mappings still work.

2. **F8 disable/re-enable**
   - disabling immediately stops mouse-camera behavior;
   - no synthetic button remains stuck down;
   - re-enabling does not jump the camera;
   - stale mouse movement is not applied afterward.

3. **Mainland → ferry → Albatross**
   - no spinning during or after arrival;
   - mouse-camera control resumes on the island.

4. **Albatross → mainland**
   - mouse-camera control remains healthy after the return transition.

5. **Criminal Scanner and other special cameras**
   - these are especially useful because they exercise non-standard camera modes.

6. **Cutscenes / scripted camera sequences**
   - the mod should defer to the game;
   - mouse control should resume cleanly afterward.

7. **Any genuine first-person-like sequence**
   - verify that the new stale-flag override does not accidentally activate there.

If all of those work, the current diagnosis and fix are strong enough that I would be comfortable calling the ferry bug fixed.

---

## 16. A useful mental model of the current mod

The easiest way to understand the new behavior is:

```text
                    ┌───────────────────┐
                    │ Game owns camera  │
                    └─────────┬─────────┘
                              │
                       normal update
                              │
                              ▼
                    ┌───────────────────┐
Mouse Raw Input ───►│ Mouse-camera mod  │
                    │ applies orbit     │
                    └───────────────────┘
```

During a transition:

```text
       loading / ferry / script detected
                    │
                    ▼
        stop accepting mouse input
                    │
                    ▼
        let game establish camera
                    │
                    ▼
      inspect resulting camera geometry
                    │
          ┌─────────┴─────────┐
          │                   │
      usable orbit       invalid/special
          │                   │
          ▼                   ▼
  rebase mod state       remain suspended
          │
          ▼
   resume mouse input
```

And when the reverse-engineered first-person flag disagrees with the visible geometry:

```text
game flag says first person
        │
        ├── geometry invalid ──► trust it; block mouse camera
        │
        └── geometry still third-person
                  │
                  ▼
           preserve evidence
                  │
          wait for load gap
                  │
                  ▼
       validate post-load orbit
                  │
                  ▼
       locally ignore stale flag
```

That is the conceptual heart of the fix.

---

## 17. Code-reading guide for a new contributor

If someone new wants to understand the current code, this is the order I would read it.

### First: `docs/mousework.md`

Read the compact technical record so you know the facts the implementation is trying to preserve.

### Second: `MouseCameraState.h`

This gives the vocabulary:

- `DesiredState`
- `InputState`
- `CameraModeGate`
- `RebaseOrbit`

### Third: `MouseCameraState.cpp`

Read the state transitions without the distraction of Win32 or hooking code.

Focus especially on:

```cpp
CameraModeGate::AllowsCamera()
CameraModeGate::RecoverAfterUpdate()
RebaseOrbit()
```

### Fourth: `dllmain.cpp`

Then see how those abstractions are applied to the actual game.

The most important function is:

```cpp
UpdateCamDetour(...)
```

Read it in phases:

1. detect update gap;
2. notice state revision changes;
3. decide whether mouse camera may be active;
4. resynchronize if needed;
5. update Raw Input;
6. consume horizontal mouse delta;
7. apply the orbit;
8. call the original game camera update.

### Fifth: `RawInput.cpp`

Only after the camera logic makes sense should you dive into the Windows message-window lifecycle.

The crucial functions are:

```cpp
register_raw_input()
init_event_queue()
is_event_queue_window()
ensure_event_queue()
Update()
```

### Finally: `tests/`

The tests are useful as executable documentation. They show what behaviors the author considers invariants.

---

## 18. Things future changes should be careful not to break

A future contributor or coding agent should preserve these properties unless there is strong evidence to change them:

- do not restore the old forced-camera memory writes casually;
- do not assume the value named `isFirstPerson` is authoritative across loading;
- do not modify that game-owned mode value just to make the mod happy;
- do not resume with pre-load mouse deltas;
- let the game establish the camera before rebasing after a transition;
- remember that the Raw Input message window is thread-owned;
- disabling the mod should genuinely stop capture, not merely stop applying rotation;
- re-enabling should resynchronize rather than immediately applying stale orbit state;
- hard-coded RVAs are executable-version-specific and should be treated as such.

---

## 19. Current conclusion

The current fix is compelling because the observed failure is now explained by concrete state transitions rather than by a generic “ferry breaks mouse input” theory.

There were at least three interacting problems:

1. the mod fought a scripted camera and caused spinning;
2. a reverse-engineered mode value remained stale after ferry travel and prevented recovery;
3. Windows Raw Input state could become invalid when loading replaced the owning camera thread.

The code now addresses each problem separately.

The latest change—preserving known third-person evidence when the game flag flips early but the camera geometry remains third-person—is particularly important because it matches the actual ordering observed during ferry travel.

The remaining work is primarily real-game validation, especially around unusual scripted and first-person camera modes. If those tests pass, this is a strong and understandable fix rather than a ferry-specific hack.
