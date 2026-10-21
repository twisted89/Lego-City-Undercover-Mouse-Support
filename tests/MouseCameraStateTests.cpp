#include "../MouseCameraState.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace
{
	int failures = 0;

	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	void CheckNear(float actual, float expected, const char* message)
	{
		Check(std::fabs(actual - expected) < 0.0001f, message);
	}
}

int main()
{
	using namespace mouse_camera;

	Check(DefaultToggleKey == 0x77, "default toggle key is F8");
	Check(IsInitialKeyDown(DefaultToggleKey, 0, DefaultToggleKey), "initial matching key-down toggles");
	Check(!IsInitialKeyDown(DefaultToggleKey, 1LL << 30, DefaultToggleKey), "auto-repeat does not toggle");
	Check(!IsInitialKeyDown(0x76, 0, DefaultToggleKey), "another key does not toggle");

	DesiredState hotkeyState;
	Check(ProcessToggleKey(hotkeyState, DefaultToggleKey, 0, DefaultToggleKey) == ToggleTransition::Disabled,
		"initial toggle key-down reports the disabled transition");
	Check(ProcessToggleKey(hotkeyState, DefaultToggleKey, 1LL << 30, DefaultToggleKey) == ToggleTransition::None,
		"toggle key auto-repeat reports no transition");
	Check(ProcessToggleKey(hotkeyState, DefaultToggleKey, 0, DefaultToggleKey) == ToggleTransition::Enabled,
		"the next initial toggle key-down reports the enabled transition");

	DesiredState desiredState;
	auto desired = desiredState.Get();
	Check(desired.enabled && !desired.focused && desired.revision == 0, "desired state starts enabled and unfocused");
	Check(!desiredState.ToggleEnabled(), "toggle publishes disabled state");
	auto disabled = desiredState.Get();
	Check(!disabled.enabled && disabled.revision > desired.revision, "disable advances desired-state revision");
	Check(desiredState.ToggleEnabled(), "second toggle publishes enabled state");
	desiredState.SetFocused(true);
	auto focused = desiredState.Get();
	Check(focused.enabled && focused.focused && focused.revision > disabled.revision, "focus is published with a newer revision");
	Check(IsMouseCameraActive(focused, focused.revision, true, true), "current enabled third-person state accepts input");
	desiredState.ToggleEnabled();
	Check(!IsMouseCameraActive(desiredState.Get(), focused.revision, true, true),
		"a newer disable transition rejects stale activation");
	desiredState.ToggleEnabled();
	const auto reenabled = desiredState.Get();
	desiredState.SetFocused(true);
	Check(desiredState.Get().revision == reenabled.revision, "publishing unchanged focus does not create a transition");

	InputState input;
	input.AddDelta(4, -3);
	Check(input.x == 0 && input.y == 0, "inactive input is ignored");
	input.SetAccepting(true);
	input.AddDelta(4, -3);
	input.AddDelta(2, 1);
	Check(input.x == 6 && input.y == -2, "active input accumulates");
	input.SetAccepting(false);
	Check(input.x == 0 && input.y == 0, "suspending clears accumulated input");
	input.AddDelta(9, 9);
	Check(input.x == 0 && input.y == 0, "suspended input remains empty");
	input.SetAccepting(true);
	Check(input.x == 0 && input.y == 0, "reactivation has no stale delta");
	input.AddDelta(3, 2);
	input.Clear();
	Check(input.accepting && input.x == 0 && input.y == 0, "consuming deltas keeps input active");

	Check(HasCameraUpdateGap(0, 1000, 500), "first update requires resynchronization");
	Check(!HasCameraUpdateGap(1000, 1500, 500), "maximum allowed update gap is continuous");
	Check(HasCameraUpdateGap(1000, 1501, 500), "long update gap requires resynchronization");
	Check(HasCameraUpdateGap(1500, 1000, 500), "backward clock requires resynchronization");

	CheckNear(AdvanceRotation(10.0f, 1, 0.25f), 9.75f,
		"mouse rotation preserves sub-degree movement");
	CheckNear(AdvanceRotation(0.125f, 1, 0.25f), 359.875f,
		"mouse rotation wraps below zero without losing its fraction");
	CheckNear(AdvanceRotation(359.875f, -1, 0.25f), 0.125f,
		"mouse rotation wraps above 360 without losing its fraction");

	CameraModeGate startupMode;
	startupMode.OnUpdateGap();
	Check(!startupMode.RecoverAfterUpdate(true, true),
		"the initial startup gap cannot bypass a first-person camera");

	CameraModeGate cameraMode;
	Check(cameraMode.AllowsCamera(false, true), "game-reported third-person mode records a recoverable camera");
	cameraMode.OnUpdateGap();
	Check(!cameraMode.AllowsCamera(true, true), "a load gap alone does not bypass first-person mode");
	Check(cameraMode.RecoverAfterUpdate(true, true),
		"a usable third-person orbit after a load gap recovers from a stale first-person flag");
	Check(cameraMode.AllowsCamera(true, true), "the recovered stale flag remains bypassed while the orbit is usable");
	Check(!cameraMode.AllowsCamera(true, false), "a real first-person orbit revokes the stale-flag bypass");
	Check(!cameraMode.RecoverAfterUpdate(true, true), "first-person mode cannot recover again without another load gap");
	Check(cameraMode.AllowsCamera(false, true), "a game-reported third-person camera is always allowed");
	Check(!cameraMode.AllowsCamera(true, true), "a stale first-person report is blocked before the load gap");
	cameraMode.OnUpdateGap();
	Check(cameraMode.RecoverAfterUpdate(true, true),
		"a first-person report immediately before the ferry gap retains the prior third-person state");

	CameraModeGate firstPersonMode;
	Check(firstPersonMode.AllowsCamera(false, true), "the first-person test starts from third-person mode");
	Check(!firstPersonMode.AllowsCamera(true, false), "a genuine first-person orbit is blocked");
	firstPersonMode.OnUpdateGap();
	Check(!firstPersonMode.RecoverAfterUpdate(true, true), "a load entered from genuine first-person mode remains blocked");

	CameraModeGate settlingMode;
	Check(settlingMode.AllowsCamera(false, true), "a settling test starts from third-person mode");
	settlingMode.OnUpdateGap();
	Check(!settlingMode.RecoverAfterUpdate(true, false), "an unsettled post-load camera is not treated as third-person");
	Check(settlingMode.RecoverAfterUpdate(true, true), "post-load recovery retries once a usable orbit appears");

	float rotation = 123.0f;
	float distance = 99.0f;
	Check(RebaseOrbit(-1.0f, -1.0f, 0.0f, 0.0f, rotation, distance), "baseline orbit rebases");
	CheckNear(rotation, 0.0f, "baseline rotation is zero");
	CheckNear(distance, 1.0f, "baseline distance is one");

	Check(RebaseOrbit(1.0f, -1.0f, 0.0f, 0.0f, rotation, distance), "quarter orbit rebases");
	CheckNear(rotation, 90.0f, "quarter orbit rotation is 90 degrees");
	CheckNear(distance, 1.0f, "quarter orbit distance is one");

	Check(RebaseOrbit(2.0f, 2.0f, 0.0f, 0.0f, rotation, distance), "scaled orbit rebases");
	CheckNear(rotation, 180.0f, "scaled orbit rotation is 180 degrees");
	CheckNear(distance, 2.0f, "scaled orbit distance is reconstructed");

	rotation = 17.0f;
	distance = 3.0f;
	Check(!RebaseOrbit(4.0f, 5.0f, 4.0f, 5.0f, rotation, distance), "zero-radius camera is rejected");
	CheckNear(rotation, 17.0f, "failed rebase preserves rotation");
	CheckNear(distance, 3.0f, "failed rebase preserves distance");
	Check(!RebaseOrbit(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f, rotation, distance), "non-finite camera is rejected");

	if (failures != 0)
	{
		std::cerr << failures << " test(s) failed\n";
		return 1;
	}

	std::cout << "MouseCameraStateTests passed\n";
	return 0;
}
