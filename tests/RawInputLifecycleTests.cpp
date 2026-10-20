#include "../RawInput.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	constexpr wchar_t RawInputClassName[] = L"RawInputCatcher";
	constexpr wchar_t RawInputWindowName[] = L"RawInputMsgWindow";

	bool Require(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			return false;
		}
		return true;
	}

	HWND FindRawInputWindowOwnedBy(DWORD threadId)
	{
		HWND window = nullptr;
		while ((window = FindWindowEx(
			HWND_MESSAGE, window, RawInputClassName, RawInputWindowName)) != nullptr)
		{
			if (GetWindowThreadProcessId(window, nullptr) == threadId)
			{
				return window;
			}
		}
		return nullptr;
	}

	bool IsMouseRegisteredTo(HWND expectedWindow)
	{
		UINT deviceCount = 0;
		if (GetRegisteredRawInputDevices(nullptr, &deviceCount, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1))
		{
			return false;
		}

		std::vector<RAWINPUTDEVICE> devices(deviceCount);
		if (deviceCount != 0 &&
			GetRegisteredRawInputDevices(devices.data(), &deviceCount, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1))
		{
			return false;
		}

		for (const auto& device : devices)
		{
			if (device.usUsagePage == 1 && device.usUsage == 2 && device.hwndTarget == expectedWindow)
			{
				return true;
			}
		}
		return false;
	}

	bool RecoversAfterOriginalThreadExits()
	{
		RawInput input(
			[](int, int) {},
			[](bool) {},
			[](unsigned short) {});

		bool initialised = false;
		HWND originalWindow = nullptr;
		std::thread originalCameraThread([&]()
		{
			initialised = input.Init();
			input.Update(true);
			originalWindow = FindRawInputWindowOwnedBy(GetCurrentThreadId());
		});
		originalCameraThread.join();

		if (!Require(initialised, "raw input should initialise on the original camera thread") ||
			!Require(originalWindow != nullptr, "initialisation should create a message-only window") ||
			!Require(!IsWindow(originalWindow), "Windows should destroy the message window when its thread exits"))
		{
			return false;
		}

		input.Update(false);
		bool passed = Require(FindRawInputWindowOwnedBy(GetCurrentThreadId()) == nullptr,
			"disabled updates should not create a queue on a transient camera thread");

		input.Update(true);

		HWND recoveredWindow = FindRawInputWindowOwnedBy(GetCurrentThreadId());
		passed = Require(recoveredWindow != nullptr && IsWindow(recoveredWindow),
			"Update should recreate a message window after the original thread exits") && passed;
		passed = Require(GetWindowLongPtr(recoveredWindow, GWLP_USERDATA) == reinterpret_cast<LONG_PTR>(&input),
			"the recovered handle should be identifiable as this RawInput window") && passed;
		passed = Require(IsMouseRegisteredTo(recoveredWindow),
			"Update should register raw mouse input to the recovered window") && passed;
		return passed;
	}

	bool MovesQueueToCurrentCameraThread()
	{
		RawInput input(
			[](int, int) {},
			[](bool) {},
			[](unsigned short) {});

		std::mutex mutex;
		std::condition_variable readyCondition;
		std::condition_variable exitCondition;
		bool ready = false;
		bool shouldExit = false;
		bool initialised = false;
		HWND originalWindow = nullptr;
		std::thread originalCameraThread([&]()
		{
			initialised = input.Init();
			input.Update(true);
			originalWindow = FindRawInputWindowOwnedBy(GetCurrentThreadId());
			{
				std::lock_guard<std::mutex> lock(mutex);
				ready = true;
			}
			readyCondition.notify_one();

			std::unique_lock<std::mutex> lock(mutex);
			exitCondition.wait(lock, [&]() { return shouldExit; });
		});

		{
			std::unique_lock<std::mutex> lock(mutex);
			readyCondition.wait(lock, [&]() { return ready; });
		}

		bool passed = Require(initialised && originalWindow != nullptr && IsWindow(originalWindow),
			"the original thread should still own a valid message window");
		input.Update(true);
		passed = Require(FindRawInputWindowOwnedBy(GetCurrentThreadId()) != nullptr,
			"Update should move the message queue to a replacement camera thread") && passed;

		{
			std::lock_guard<std::mutex> lock(mutex);
			shouldExit = true;
		}
		exitCondition.notify_one();
		originalCameraThread.join();
		return passed;
	}
}

int main()
{
	if (!RecoversAfterOriginalThreadExits() || !MovesQueueToCurrentCameraThread())
	{
		return 1;
	}

	std::cout << "RawInput lifecycle tests passed\n";
	return 0;
}
