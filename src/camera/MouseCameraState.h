#pragma once

#include <atomic>
#include <cstdint>

namespace mouse_camera
{
	constexpr unsigned short DefaultToggleKey = 0x77; // VK_F8

	struct DesiredStateSnapshot
	{
		bool enabled;
		bool focused;
		std::uint64_t revision;
	};

	enum class ToggleTransition
	{
		None,
		Enabled,
		Disabled
	};

	class DesiredState
	{
	public:
		DesiredState();

		bool ToggleEnabled();
		void SetFocused(bool focused);
		DesiredStateSnapshot Get() const;

	private:
		std::atomic<std::uint64_t> state;
	};

	ToggleTransition ProcessToggleKey(DesiredState& state, std::uintptr_t pressedKey,
		std::intptr_t lParam, unsigned short toggleKey);
	bool IsMouseCameraActive(const DesiredStateSnapshot& state, std::uint64_t expectedRevision,
		bool thirdPerson, bool positionsValid);

	struct InputState
	{
		bool accepting = false;
		int x = 0;
		int y = 0;

		void SetAccepting(bool value);
		void AddDelta(int deltaX, int deltaY);
		void Clear();
	};

	bool IsInitialKeyDown(std::uintptr_t pressedKey, std::intptr_t lParam, unsigned short toggleKey);
	bool HasCameraUpdateGap(std::uint64_t previousTick, std::uint64_t currentTick, std::uint64_t maximumGap);
	float AdvanceRotation(float rotation, int horizontalMouseDelta, float sensitivity);

	// The game's first-person flag can remain set after ferry loads. This gate permits a
	// local override only when a load gap follows a known third-person camera and the
	// resulting camera/target positions still describe a usable orbit.
	class CameraModeGate
	{
	public:
		void OnUpdateGap();
		bool AllowsCamera(bool reportedFirstPerson, bool orbitUsable);
		bool RecoverAfterUpdate(bool reportedFirstPerson, bool orbitUsable);

	private:
		// Armed by a camera-update gap only when third-person geometry was previously observed.
		bool recoveryPending = false;
		// Bypasses a stale game flag locally; the game-owned flag is never modified.
		bool staleFirstPersonOverride = false;
		// Retained when the ferry raises its mode flag before the actual update gap.
		bool thirdPersonObserved = false;
	};

	bool RebaseOrbit(float cameraX, float cameraZ, float playerX, float playerZ, float& rotation, float& distance);
}
