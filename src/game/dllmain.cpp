#include <windows.h>
#include <tchar.h>
#include <stdint.h>
#include <MinHook.h>
#include <stdio.h>
#include <math.h>
#include "vector3.h"
#include "Proxy.h"
#include "RawInput.h"
#include "MouseCameraState.h"
#include "ini.h"

#include <mutex>
#include <type_traits>

#define DEBUG_PRINTF(...) {char cad[512]; sprintf(cad, __VA_ARGS__);  OutputDebugStringA(cad);}

LRESULT CALLBACK WndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

struct ButtonEvent
{
	unsigned short vkey;
	bool pressed;
};

typedef void(__fastcall* UPDATECAM)(__int64, float*, float*, float, int, int, char, char, __int64);
static_assert(std::is_void<std::result_of<UPDATECAM(__int64, float*, float*, float, int, int, char, char, __int64)>::type>::value,
	"The game's camera update routine has no return value");
UPDATECAM fpUpdateCam = nullptr;

typedef int64_t(__fastcall* KEYPRESSFUNC)(int);
KEYPRESSFUNC fpKeyPress = nullptr;
unsigned char* keyStates = nullptr;
unsigned char* keyCounter = nullptr;

WNDPROC fpWndProc = nullptr;

struct WindowPlacement
{
	LONG_PTR style = 0;
	LONG_PTR extendedStyle = 0;
	RECT rect = {};
	bool valid = false;
};

struct WindowModeState
{
	HWND window = nullptr;
	bool initialized = false;
	bool windowed = false;
	WindowPlacement windowedPlacement;
	WindowPlacement fullscreenPlacement;
};

WindowModeState windowModeState;

float sensitivity = 1, distance = 1;
int* isFirstPerson = nullptr;
float rotation = 0;
mouse_camera::InputState mouseInput;
mouse_camera::DesiredState desiredState;
mouse_camera::CameraModeGate cameraModeGate;
std::mutex mouseInputMutex;
bool initialised = false;
bool needsCameraResync = true;
bool leftClickDown = false, rightClickDown = false;
unsigned short leftClickKey = 0x51, rightClickKey = 0x45;
unsigned short toggleKey = mouse_camera::DefaultToggleKey;
std::uint64_t lastCameraUpdateTick = 0;
std::uint64_t lastDesiredStateRevision = UINT64_MAX;

static constexpr std::uint64_t MaximumCameraUpdateGapMs = 500;



static bool GetWindowPlacement(HWND window, WindowPlacement& placement)
{
	SetLastError(ERROR_SUCCESS);
	const LONG_PTR style = GetWindowLongPtr(window, GWL_STYLE);
	if (style == 0 && GetLastError() != ERROR_SUCCESS)
	{
		DEBUG_PRINTF("Unable to read game window style: %lu\n", GetLastError());
		return false;
	}

	SetLastError(ERROR_SUCCESS);
	const LONG_PTR extendedStyle = GetWindowLongPtr(window, GWL_EXSTYLE);
	if (extendedStyle == 0 && GetLastError() != ERROR_SUCCESS)
	{
		DEBUG_PRINTF("Unable to read game window extended style: %lu\n", GetLastError());
		return false;
	}

	RECT rect = {};
	if (!GetWindowRect(window, &rect))
	{
		DEBUG_PRINTF("Unable to read game window bounds: %lu\n", GetLastError());
		return false;
	}

	placement.style = style;
	placement.extendedStyle = extendedStyle;
	placement.rect = rect;
	placement.valid = true;
	return true;
}



static bool ApplyWindowPlacement(HWND window, const WindowPlacement& placement)
{
	if (!placement.valid)
	{
		return false;
	}

	SetLastError(ERROR_SUCCESS);
	if (SetWindowLongPtr(window, GWL_STYLE, placement.style) == 0 &&
		GetLastError() != ERROR_SUCCESS)
	{
		DEBUG_PRINTF("Unable to set game window style: %lu\n", GetLastError());
		return false;
	}

	SetLastError(ERROR_SUCCESS);
	if (SetWindowLongPtr(window, GWL_EXSTYLE, placement.extendedStyle) == 0 &&
		GetLastError() != ERROR_SUCCESS)
	{
		DEBUG_PRINTF("Unable to set game window extended style: %lu\n", GetLastError());
		return false;
	}

	const HWND insertAfter = (placement.extendedStyle & WS_EX_TOPMOST) != 0
		? HWND_TOPMOST : HWND_NOTOPMOST;
	if (!SetWindowPos(window, insertAfter, placement.rect.left, placement.rect.top,
		placement.rect.right - placement.rect.left, placement.rect.bottom - placement.rect.top,
		SWP_FRAMECHANGED | SWP_SHOWWINDOW))
	{
		DEBUG_PRINTF("Unable to resize game window: %lu\n", GetLastError());
		return false;
	}
	return true;
}



static bool GetMonitorBounds(HWND window, RECT& monitorBounds, RECT& workArea)
{
	const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	MONITORINFO monitorInfo = {};
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (!GetMonitorInfo(monitor, &monitorInfo))
	{
		DEBUG_PRINTF("Unable to read game monitor bounds: %lu\n", GetLastError());
		return false;
	}

	monitorBounds = monitorInfo.rcMonitor;
	workArea = monitorInfo.rcWork;
	return true;
}



static bool MakeWindowedPlacement(HWND window, const WindowPlacement& current,
	WindowPlacement& windowed)
{
	RECT monitorBounds = {};
	RECT workArea = {};
	if (!GetMonitorBounds(window, monitorBounds, workArea))
	{
		return false;
	}

	windowed.style = (current.style & ~(WS_POPUP | WS_CHILD)) | WS_OVERLAPPEDWINDOW;
	windowed.extendedStyle = current.extendedStyle & ~WS_EX_TOPMOST;

	const LONG monitorWidth = monitorBounds.right - monitorBounds.left;
	const LONG monitorHeight = monitorBounds.bottom - monitorBounds.top;
	const LONG workWidth = workArea.right - workArea.left;
	const LONG workHeight = workArea.bottom - workArea.top;
	if (monitorWidth <= 0 || monitorHeight <= 0 || workWidth <= 0 || workHeight <= 0)
	{
		DEBUG_PRINTF("Invalid game monitor dimensions\n");
		return false;
	}

	LONG clientWidth = workWidth * 4 / 5;
	LONG clientHeight = workHeight * 4 / 5;
	if (static_cast<double>(clientWidth) / clientHeight >
		static_cast<double>(monitorWidth) / monitorHeight)
	{
		clientWidth = static_cast<LONG>(
			static_cast<double>(clientHeight) * monitorWidth / monitorHeight);
	}
	else
	{
		clientHeight = static_cast<LONG>(
			static_cast<double>(clientWidth) * monitorHeight / monitorWidth);
	}

	RECT rect = { 0, 0, clientWidth, clientHeight };
	if (!AdjustWindowRectEx(&rect, static_cast<DWORD>(windowed.style), FALSE,
		static_cast<DWORD>(windowed.extendedStyle)))
	{
		DEBUG_PRINTF("Unable to calculate game window bounds: %lu\n", GetLastError());
		return false;
	}

	const LONG windowWidth = rect.right - rect.left;
	const LONG windowHeight = rect.bottom - rect.top;
	rect.left = workArea.left + (workWidth - windowWidth) / 2;
	rect.top = workArea.top + (workHeight - windowHeight) / 2;
	rect.right = rect.left + windowWidth;
	rect.bottom = rect.top + windowHeight;

	windowed.rect = rect;
	windowed.valid = true;
	return true;
}



static bool MakeFullscreenPlacement(HWND window, const WindowPlacement& current, WindowPlacement& fullscreen)
{
	RECT workArea = {};
	if (!GetMonitorBounds(window, fullscreen.rect, workArea))
	{
		return false;
	}

	fullscreen.style = (current.style & ~WS_OVERLAPPEDWINDOW & ~WS_CHILD) | WS_POPUP;
	fullscreen.extendedStyle = current.extendedStyle | WS_EX_TOPMOST;
	fullscreen.extendedStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
		WS_EX_DLGMODALFRAME);
	fullscreen.valid = true;
	return true;
}



static void ToggleWindowMode(HWND window)
{
	if (!IsWindow(window))
	{
		DEBUG_PRINTF("Cannot toggle mode for an invalid game window\n");
		return;
	}

	if (windowModeState.window != window)
	{
		windowModeState = {};
		windowModeState.window = window;
	}

	WindowPlacement current;
	if (!GetWindowPlacement(window, current))
	{
		return;
	}

	if (!windowModeState.initialized)
	{
		windowModeState.windowed = (current.style & WS_CAPTION) != 0;
		windowModeState.initialized = true;
		if (windowModeState.windowed)
		{
			windowModeState.windowedPlacement = current;
		}
		else
		{
			windowModeState.fullscreenPlacement = current;
		}
	}

	if (windowModeState.windowed)
	{
		windowModeState.windowedPlacement = current;
		if (!windowModeState.fullscreenPlacement.valid &&
			!MakeFullscreenPlacement(window, current, windowModeState.fullscreenPlacement))
		{
			return;
		}
		if (ApplyWindowPlacement(window, windowModeState.fullscreenPlacement))
		{
			windowModeState.windowed = false;
		}
		return;
	}

	windowModeState.fullscreenPlacement = current;
	if (!windowModeState.windowedPlacement.valid &&
		!MakeWindowedPlacement(window, current, windowModeState.windowedPlacement))
	{
		return;
	}
	if (ApplyWindowPlacement(window, windowModeState.windowedPlacement))
	{
		windowModeState.windowed = true;
	}
}



static void OnMouseInput(int mx, int my)
{
	std::lock_guard<std::mutex> lock(mouseInputMutex);
	mouseInput.AddDelta(mx, my);
}



static void OnMouseScroll(bool up)
{
	std::lock_guard<std::mutex> lock(mouseInputMutex);
	if (mouseInput.accepting)
	{
		distance += up ? -0.1f : 0.1f;
	}
}



static void SendButton(unsigned short key, bool down)
{
	//DEBUG_PRINTF("Sending key %d %s\n", key, down ? "DOWN" : "UP");
	INPUT input_config = {};
	input_config.type = INPUT_KEYBOARD;
	input_config.ki.wScan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
	input_config.ki.dwFlags = (KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP));
	SendInput(1, &input_config, sizeof(input_config));
}



static void OnMouseButton(unsigned short buttonFlags)
{
	std::lock_guard<std::mutex> lock(mouseInputMutex);
	if (!mouseInput.accepting)
	{
		return;
	}

	if (buttonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
		if (!leftClickDown)
		{
			SendButton(leftClickKey, true);
			leftClickDown = true;
		}
	}
	if (buttonFlags & RI_MOUSE_LEFT_BUTTON_UP) {
		if (leftClickDown)
		{
			SendButton(leftClickKey, false);
			leftClickDown = false;
		}
	}
	if (buttonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
		if (!rightClickDown)
		{
			SendButton(rightClickKey, true);
			rightClickDown = true;
		}
	}
	if (buttonFlags & RI_MOUSE_RIGHT_BUTTON_UP) {
		if (rightClickDown)
		{
			SendButton(rightClickKey, false);
			rightClickDown = false;
		}
	}
}



static void ClearMouseCameraInput()
{
	bool releaseLeft;
	bool releaseRight;
	{
		std::lock_guard<std::mutex> lock(mouseInputMutex);
		mouseInput.SetAccepting(false);
		releaseLeft = leftClickDown;
		releaseRight = rightClickDown;
		leftClickDown = false;
		rightClickDown = false;
	}

	if (releaseLeft)
	{
		SendButton(leftClickKey, false);
	}
	if (releaseRight)
	{
		SendButton(rightClickKey, false);
	}
}



static void SetMouseCameraInputAccepting(bool accepting)
{
	std::lock_guard<std::mutex> lock(mouseInputMutex);
	mouseInput.SetAccepting(accepting);
}



static int ConsumeHorizontalMouseDelta()
{
	std::lock_guard<std::mutex> lock(mouseInputMutex);
	const int delta = mouseInput.x;
	mouseInput.Clear();
	return delta;
}



Vector3 static RotateCam(Vector3 cam, Vector3 point, double angle)
{
	static constexpr double PI = 3.14159265358979323846f;

	double angInRad = (angle * PI) / 180;

	double relativeX = cam.x - point.x;
	double relativeY = cam.y - point.y;

	double nRelativeX = (relativeX * cos(angInRad)) - (relativeY * sin(angInRad));
	relativeY = (relativeX * sin(angInRad)) + (relativeY * cos(angInRad));
	return Vector3(static_cast<float>(point.x + nRelativeX), static_cast<float>(point.y + relativeY), 0);
}

RawInput input(OnMouseInput, OnMouseScroll, OnMouseButton);



static void SuspendMouseCameraInput()
{
	ClearMouseCameraInput();
	if (initialised)
	{
		input.Update(false);
	}
}



static void SuspendMouseCameraInputImmediately()
{
	ClearMouseCameraInput();
	input.DisableCapture();
}



static bool HasUsableOrbit(float* camPos, float* playerPos)
{
	// Validate camera geometry independently of the game mode flag. Ferry travel can
	// leave that flag stale even though these positions still form a third-person orbit.
	float ignoredRotation = 0.0f;
	float ignoredDistance = 0.0f;
	return camPos != nullptr && playerPos != nullptr &&
		mouse_camera::RebaseOrbit(camPos[0], camPos[2], playerPos[0], playerPos[2],
			ignoredRotation, ignoredDistance);
}



LRESULT CALLBACK WndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
	// Raw input must target a window whose lifetime is independent of the camera
	// worker thread. The game's own window supplies that stable target, and this
	// existing hook lets us consume WM_INPUT without owning another message queue.
	input.SetTargetWindow(hWnd);
	if (Msg == WM_INPUT)
	{
		input.HandleRawInput(wParam, lParam);
	}

	if ((Msg == WM_KEYDOWN || Msg == WM_SYSKEYDOWN) && wParam == VK_F9)
	{
		if ((static_cast<std::uintptr_t>(lParam) & (1ULL << 30)) == 0)
		{
			ToggleWindowMode(hWnd);
		}
		return 0;
	}
	if ((Msg == WM_KEYUP || Msg == WM_SYSKEYUP) && wParam == VK_F9)
	{
		return 0;
	}

	switch (Msg)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (mouse_camera::ProcessToggleKey(desiredState, wParam, lParam, toggleKey) ==
			mouse_camera::ToggleTransition::Disabled)
		{
			SuspendMouseCameraInputImmediately();
		}
		break;
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			desiredState.SetFocused(false);
			SuspendMouseCameraInputImmediately();
		}
		else
		{
			desiredState.SetFocused(true);
		}
		break;
	}
	return fpWndProc(hWnd, Msg, wParam, lParam);
}



void __fastcall UpdateCamDetour(__int64 a1, float* camPos, float* playerPos , float a4, int a5, int a6, char a7, char a8, __int64 a9)
{
	const std::uint64_t currentTick = GetTickCount64();
	if (mouse_camera::HasCameraUpdateGap(lastCameraUpdateTick, currentTick, MaximumCameraUpdateGapMs))
	{
		// Loading and fast travel stop this hook long enough to make accumulated mouse
		// deltas and the pre-load camera orbit unsafe to reuse.
		needsCameraResync = true;
		cameraModeGate.OnUpdateGap();
	}
	lastCameraUpdateTick = currentTick;

	auto desired = desiredState.Get();
	if (desired.revision != lastDesiredStateRevision)
	{
		lastDesiredStateRevision = desired.revision;
		needsCameraResync = true;
	}

	const bool positionsValid = camPos != nullptr && playerPos != nullptr;
	const bool baseActive = mouse_camera::IsMouseCameraActive(
		desired, lastDesiredStateRevision, true, positionsValid);
	if (!baseActive)
	{
		needsCameraResync = true;
		SuspendMouseCameraInput();
		fpUpdateCam(a1, camPos, playerPos, a4, a5, a6, a7, a8, a9);
		return;
	}

	const bool cameraModeAllowed = isFirstPerson != nullptr && cameraModeGate.AllowsCamera(
		*isFirstPerson != 0, HasUsableOrbit(camPos, playerPos));
	if (!cameraModeAllowed)
	{
		needsCameraResync = true;
	}

	if (cameraModeAllowed && GetAsyncKeyState(VK_HOME)) {
		sensitivity += 0.005f;
	}
	if (cameraModeAllowed && GetAsyncKeyState(VK_END)) {
		sensitivity -= 0.005f;
		if (sensitivity <= 0)
			sensitivity = 0.01f;
	}

	if (needsCameraResync)
	{
		// Let the game establish its post-transition camera before deriving our orbit.
		// Suspending first also prevents pre-load mouse deltas from causing a jump.
		SuspendMouseCameraInput();
		fpUpdateCam(a1, camPos, playerPos, a4, a5, a6, a7, a8, a9);
		desired = desiredState.Get();
		const bool baseStillActive = mouse_camera::IsMouseCameraActive(
			desired, lastDesiredStateRevision, true, camPos != nullptr && playerPos != nullptr);
		if (!baseStillActive)
		{
			lastDesiredStateRevision = desired.revision;
			needsCameraResync = true;
		}
		else
		{
			float rebasedRotation = rotation;
			float rebasedDistance = distance;
			const bool orbitUsable = mouse_camera::RebaseOrbit(
				camPos[0], camPos[2], playerPos[0], playerPos[2], rebasedRotation, rebasedDistance);
			const bool modeRecovered = isFirstPerson != nullptr &&
				cameraModeGate.RecoverAfterUpdate(*isFirstPerson != 0, orbitUsable);
			if (!orbitUsable || !modeRecovered)
			{
				return;
			}

			rotation = rebasedRotation;
			distance = rebasedDistance;
			if (!initialised)
			{
				initialised = input.Init();
			}
			if (!initialised)
			{
				ClearMouseCameraInput();
				return;
			}

			// Re-register against the persistent game HWND after the post-load orbit has
			// been rebased. No camera-thread-owned window is involved.
			input.Update(true);
			SetMouseCameraInputAccepting(true);
			desired = desiredState.Get();
			const bool modeStillAllowed = isFirstPerson != nullptr && cameraModeGate.AllowsCamera(
				*isFirstPerson != 0, HasUsableOrbit(camPos, playerPos));
			if (mouse_camera::IsMouseCameraActive(desired, lastDesiredStateRevision,
				true, camPos != nullptr && playerPos != nullptr) && modeStillAllowed)
			{
				needsCameraResync = false;
			}
			else
			{
				lastDesiredStateRevision = desired.revision;
				needsCameraResync = true;
				SuspendMouseCameraInput();
			}
		}
		return;
	}

	if (!initialised)
	{
		needsCameraResync = true;
		ClearMouseCameraInput();
		fpUpdateCam(a1, camPos, playerPos, a4, a5, a6, a7, a8, a9);
		return;
	}

	SetMouseCameraInputAccepting(true);
	input.Update(true);
	desired = desiredState.Get();
	const bool modeStillAllowed = isFirstPerson != nullptr && cameraModeGate.AllowsCamera(
		*isFirstPerson != 0, HasUsableOrbit(camPos, playerPos));
	const bool stillActive = mouse_camera::IsMouseCameraActive(
		desired, lastDesiredStateRevision, true, camPos != nullptr && playerPos != nullptr) && modeStillAllowed;
	if (!stillActive)
	{
		lastDesiredStateRevision = desired.revision;
		needsCameraResync = true;
		SuspendMouseCameraInput();
		fpUpdateCam(a1, camPos, playerPos, a4, a5, a6, a7, a8, a9);
		return;
	}

	const int horizontalMouseDelta = ConsumeHorizontalMouseDelta();
	if (horizontalMouseDelta != 0)
	{
		// Preserve fractional degrees between frames. Integer modulo quantized every raw
		// mouse count to whole degrees and made low-speed movement visibly asymmetric.
		rotation = mouse_camera::AdvanceRotation(rotation, horizontalMouseDelta, sensitivity);
	}
	auto rotatedCam = RotateCam(Vector3(playerPos[0] - distance, playerPos[2] - distance, 0), Vector3(playerPos[0], playerPos[2], 0), rotation);

	camPos[0] = rotatedCam.x;
	camPos[2] = rotatedCam.y;

	//DEBUG_PRINTF("ROTATION %.6f %.6f\n", rotatedCam.x, rotatedCam.y);

	fpUpdateCam(a1, camPos, playerPos, a4, a5, a6, a7, a8, a9);
}



BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		//MessageBoxW(NULL, L"Loaded", L"Debug", MB_OK);

		uintptr_t moduleBase = (uintptr_t)GetModuleHandle(nullptr);

		TCHAR modulePath[MAX_PATH];
		GetModuleFileName(hModule, modulePath, MAX_PATH);

		// Extract directory from module path
		TCHAR moduleDir[MAX_PATH];
		_tcscpy_s(moduleDir, MAX_PATH, modulePath);
		TCHAR* lastBackslash = _tcsrchr(moduleDir, _T('\\'));
		if (lastBackslash) {
			*lastBackslash = _T('\0');
		}

		// Check if gog.ico exists
		TCHAR launchPath[MAX_PATH];
		_sntprintf_s(launchPath, MAX_PATH, _TRUNCATE, _T("%s\\gog.ico"), moduleDir);

		const TCHAR* steam_dllPath = _T("proxy\\steam_api64_steam.dll");
		const TCHAR* gog_dllPath = _T("proxy\\steam_api64_gog.dll");

		// Choose DLL path based on gog icon
		const TCHAR* dllPath = (GetFileAttributes(launchPath) != INVALID_FILE_ATTRIBUTES) ? gog_dllPath : steam_dllPath;

		steam_api64.dll = LoadLibrary(dllPath);
		setupFunctions();

		try {
			mINI::INIFile file("lcu.ini");
			mINI::INIStructure ini;

			if (file.read(ini))
			{
				if (ini.has("settings"))
				{
					auto& collection = ini["settings"];
					if (collection.has("sensitivity"))
					{
						sensitivity = std::stof(collection["sensitivity"]);
					}
					if (collection.has("leftclick"))
					{
						leftClickKey = static_cast<unsigned short>(std::stoi(collection["leftclick"], 0, 16));
					}
					if (collection.has("rightclick"))
					{
						rightClickKey = static_cast<unsigned short>(std::stoi(collection["rightclick"], 0, 16));
					}
					if (collection.has("togglekey"))
					{
						const int configuredToggleKey = std::stoi(collection["togglekey"], 0, 16);
						if (configuredToggleKey > 0 && configuredToggleKey <= 0xFF)
						{
							toggleKey = static_cast<unsigned short>(configuredToggleKey);
						}
					}
				}
			}
		}
		catch (std::exception& ex)
		{
			DEBUG_PRINTF("Error %s", ex.what());
		}

		if (MH_Initialize() != MH_OK)
		{
			return FALSE;
		}

		isFirstPerson = reinterpret_cast<int*>(moduleBase + 0x18152A4);
		auto cameraUpdateFP = reinterpret_cast<LPVOID>(moduleBase + 0x991500);
		auto wndProcFP = reinterpret_cast<LPVOID>(moduleBase + 0x449410);
		fpKeyPress = reinterpret_cast<KEYPRESSFUNC>(moduleBase + 0x449940);
		keyStates = reinterpret_cast<unsigned char*>(moduleBase + 0x1C2D870);
		keyCounter = reinterpret_cast<unsigned char*>(moduleBase + 0x1C2D66C);

		if (MH_CreateHook(cameraUpdateFP, &UpdateCamDetour,
			reinterpret_cast<LPVOID*>(&fpUpdateCam)) != MH_OK)
		{
			return FALSE;
		}

		if (MH_EnableHook(cameraUpdateFP) != MH_OK)
		{
			return FALSE;
		}

		if (MH_CreateHook(wndProcFP, &WndProc,
			reinterpret_cast<LPVOID*>(&fpWndProc)) != MH_OK)
		{
			return FALSE;
		}

		if (MH_EnableHook(wndProcFP) != MH_OK)
		{
			return FALSE;
		}
	}
	break;
	case DLL_PROCESS_DETACH:
		MH_Uninitialize();
		FreeLibrary(steam_api64.dll);

		try {
			mINI::INIFile file("lcu.ini");
			mINI::INIStructure ini;

			char leftclickHex[16];
			char rightclickHex[16];
			char toggleKeyHex[16];
			sprintf(leftclickHex, "%X", leftClickKey);
			sprintf(rightclickHex, "%X", rightClickKey);
			sprintf(toggleKeyHex, "%X", toggleKey);

			ini["settings"]["sensitivity"] = std::to_string(sensitivity);

			ini["settings"]["leftclick"] = "0x" + std::string(leftclickHex);
			ini["settings"]["rightclick"] = "0x" +  std::string(rightclickHex);
			ini["settings"]["togglekey"] = "0x" + std::string(toggleKeyHex);

			file.write(ini);
		}
		catch (std::exception& ex)
		{
			DEBUG_PRINTF("Error %s", ex.what());
		}

		break;
	}

	return TRUE;
}
