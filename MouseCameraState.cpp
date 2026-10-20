#include "MouseCameraState.h"

#include <cmath>
#include <limits>

namespace mouse_camera
{
	namespace
	{
		constexpr std::uint64_t EnabledMask = 1;
		constexpr std::uint64_t FocusedMask = 2;
		constexpr std::uint64_t StateMask = EnabledMask | FocusedMask;
		constexpr std::uint64_t RevisionIncrement = StateMask + 1;
	}

	DesiredState::DesiredState() :
		state(EnabledMask)
	{
	}

	bool DesiredState::ToggleEnabled()
	{
		auto current = state.load(std::memory_order_acquire);
		for (;;)
		{
			const auto desired = ((current & ~StateMask) + RevisionIncrement) |
				((current ^ EnabledMask) & StateMask);
			if (state.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				return (desired & EnabledMask) != 0;
			}
		}
	}

	void DesiredState::SetFocused(bool focused)
	{
		auto current = state.load(std::memory_order_acquire);
		for (;;)
		{
			if (((current & FocusedMask) != 0) == focused)
			{
				return;
			}

			const auto desired = ((current & ~StateMask) + RevisionIncrement) |
				(current & EnabledMask) | (focused ? FocusedMask : 0);
			if (state.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				return;
			}
		}
	}

	DesiredStateSnapshot DesiredState::Get() const
	{
		const auto current = state.load(std::memory_order_acquire);
		return {
			(current & EnabledMask) != 0,
			(current & FocusedMask) != 0,
			current / RevisionIncrement
		};
	}

	ToggleTransition ProcessToggleKey(DesiredState& state, std::uintptr_t pressedKey,
		std::intptr_t lParam, unsigned short toggleKey)
	{
		if (!IsInitialKeyDown(pressedKey, lParam, toggleKey))
		{
			return ToggleTransition::None;
		}

		return state.ToggleEnabled() ? ToggleTransition::Enabled : ToggleTransition::Disabled;
	}

	bool IsMouseCameraActive(const DesiredStateSnapshot& state, std::uint64_t expectedRevision,
		bool thirdPerson, bool positionsValid)
	{
		return state.revision == expectedRevision && state.enabled && state.focused && thirdPerson && positionsValid;
	}

	void InputState::SetAccepting(bool value)
	{
		accepting = value;
		if (!accepting)
		{
			Clear();
		}
	}

	void InputState::AddDelta(int deltaX, int deltaY)
	{
		if (accepting)
		{
			x += deltaX;
			y += deltaY;
		}
	}

	void InputState::Clear()
	{
		x = 0;
		y = 0;
	}

	bool IsInitialKeyDown(std::uintptr_t pressedKey, std::intptr_t lParam, unsigned short toggleKey)
	{
		constexpr std::uintptr_t PreviousKeyState = 1ULL << 30;
		return pressedKey == toggleKey && (static_cast<std::uintptr_t>(lParam) & PreviousKeyState) == 0;
	}

	bool HasCameraUpdateGap(std::uint64_t previousTick, std::uint64_t currentTick, std::uint64_t maximumGap)
	{
		return previousTick == 0 || currentTick < previousTick || currentTick - previousTick > maximumGap;
	}

	void CameraModeGate::OnUpdateGap()
	{
		recoveryPending = thirdPersonObserved;
		staleFirstPersonOverride = false;
		thirdPersonObserved = false;
	}

	bool CameraModeGate::AllowsCamera(bool reportedFirstPerson, bool orbitUsable)
	{
		if (!reportedFirstPerson)
		{
			recoveryPending = false;
			staleFirstPersonOverride = false;
			thirdPersonObserved = true;
			return true;
		}

		if (!staleFirstPersonOverride)
		{
			thirdPersonObserved = false;
			return false;
		}

		if (!orbitUsable)
		{
			staleFirstPersonOverride = false;
			recoveryPending = false;
			thirdPersonObserved = false;
			return false;
		}

		thirdPersonObserved = true;
		return true;
	}

	bool CameraModeGate::RecoverAfterUpdate(bool reportedFirstPerson, bool orbitUsable)
	{
		if (!reportedFirstPerson)
		{
			return AllowsCamera(false, orbitUsable);
		}

		if (recoveryPending && orbitUsable)
		{
			recoveryPending = false;
			staleFirstPersonOverride = true;
			thirdPersonObserved = true;
			return true;
		}

		return AllowsCamera(true, orbitUsable);
	}

	bool RebaseOrbit(float cameraX, float cameraZ, float playerX, float playerZ, float& rotation, float& distance)
	{
		if (!std::isfinite(cameraX) || !std::isfinite(cameraZ) ||
			!std::isfinite(playerX) || !std::isfinite(playerZ))
		{
			return false;
		}

		const double deltaX = static_cast<double>(cameraX) - playerX;
		const double deltaZ = static_cast<double>(cameraZ) - playerZ;
		const double radius = std::hypot(deltaX, deltaZ);
		if (!std::isfinite(radius) || radius <= std::numeric_limits<float>::epsilon())
		{
			return false;
		}

		constexpr double Pi = 3.14159265358979323846;
		constexpr double InverseSqrtTwo = 0.70710678118654752440;
		double newRotation = (std::atan2(deltaZ, deltaX) * 180.0 / Pi) + 135.0;
		newRotation = std::fmod(newRotation, 360.0);
		if (newRotation < 0.0)
		{
			newRotation += 360.0;
		}

		const double newDistance = radius * InverseSqrtTwo;
		if (!std::isfinite(newRotation) || !std::isfinite(newDistance))
		{
			return false;
		}

		rotation = static_cast<float>(newRotation);
		distance = static_cast<float>(newDistance);
		return true;
	}
}
