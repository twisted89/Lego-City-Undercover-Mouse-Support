#include "../RawInput.h"

#include <iostream>
#include <thread>
#include <vector>

namespace
{
	constexpr wchar_t TestWindowClass[] = L"RawInputStableTargetTest";

	bool Require(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			return false;
		}
		return true;
	}

	LRESULT CALLBACK TestWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		return DefWindowProc(window, message, wParam, lParam);
	}

	HWND CreateTestWindow()
	{
		static ATOM windowClass = 0;
		if (windowClass == 0)
		{
			WNDCLASSEXW definition = {};
			definition.cbSize = sizeof(definition);
			definition.lpfnWndProc = TestWndProc;
			definition.lpszClassName = TestWindowClass;
			definition.hInstance = GetModuleHandle(nullptr);
			windowClass = RegisterClassExW(&definition);
		}

		return CreateWindowExW(0, TestWindowClass, L"RawInput target", 0,
			0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), nullptr);
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

	bool RegistersToPersistentGameWindow()
	{
		HWND target = CreateTestWindow();
		if (!Require(target != nullptr, "the stable target window should be created"))
		{
			return false;
		}

		RawInput input(
			[](int, int) {},
			[](bool) {},
			[](unsigned short) {});
		input.SetTargetWindow(target);
		bool initialised = false;
		std::thread transientCameraWorker([&]()
		{
			initialised = input.Init();
			input.Update(true);
		});
		transientCameraWorker.join();

		bool passed = Require(initialised, "raw input should initialise without creating its own window");
		passed = Require(IsWindow(target), "the target should survive after the camera worker exits") && passed;
		passed = Require(IsMouseRegisteredTo(target), "raw mouse input should target the persistent game window") && passed;

		input.Update(false);
		passed = Require(!IsMouseRegisteredTo(target), "disabling should unregister the persistent target") && passed;
		DestroyWindow(target);
		return passed;
	}

	bool RetargetsAfterWindowReplacement()
	{
		RawInput input(
			[](int, int) {},
			[](bool) {},
			[](unsigned short) {});
		if (!Require(input.Init(), "raw input should initialise before retargeting"))
		{
			return false;
		}

		HWND original = CreateTestWindow();
		input.SetTargetWindow(original);
		input.Update(true);
		bool passed = Require(IsMouseRegisteredTo(original), "the original game window should receive raw input");
		DestroyWindow(original);
		input.Update(true);

		HWND replacement = CreateTestWindow();
		input.SetTargetWindow(replacement);
		input.Update(true);
		passed = Require(IsWindow(replacement), "the replacement game window should remain valid") && passed;
		passed = Require(IsMouseRegisteredTo(replacement), "raw input should move to the replacement game window") && passed;

		input.Update(false);
		DestroyWindow(replacement);
		return passed;
	}
}

int main()
{
	if (!RegistersToPersistentGameWindow() || !RetargetsAfterWindowReplacement())
	{
		return 1;
	}

	std::cout << "RawInput stable-target tests passed\n";
	return 0;
}
