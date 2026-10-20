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

	class CameraModeGate
	{
	public:
		void OnUpdateGap();
		bool AllowsCamera(bool reportedFirstPerson, bool orbitUsable);
		bool RecoverAfterUpdate(bool reportedFirstPerson, bool orbitUsable);

	private:
		bool recoveryPending = false;
		bool staleFirstPersonOverride = false;
		bool thirdPersonObserved = false;
	};

	bool RebaseOrbit(float cameraX, float cameraZ, float playerX, float playerZ, float& rotation, float& distance);
}
