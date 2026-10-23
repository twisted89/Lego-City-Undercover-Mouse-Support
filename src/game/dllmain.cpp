#include <windows.h>
#include <tchar.h>
#include <stdint.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <MinHook.h>
#include <stdio.h>
#include <math.h>
#include "vector3.h"
#include "Proxy.h"
#include "RawInput.h"
#include "MouseCameraState.h"
#include "Settings.h"
#include "ini.h"

#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
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
lcu_config::Settings configSettings;
float distance = 1;
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

	const LONG workWidth = workArea.right - workArea.left;
	const LONG workHeight = workArea.bottom - workArea.top;
	if (workWidth <= 0 || workHeight <= 0)
	{
		DEBUG_PRINTF("Invalid game monitor work area\n");
		return false;
	}

	// Determine the normal decorated-frame overhead before calculating the
	// largest client rectangle that can fit entirely in the monitor work area.
	RECT frameBounds = {};
	if (!AdjustWindowRectEx(&frameBounds, static_cast<DWORD>(windowed.style), FALSE,
		static_cast<DWORD>(windowed.extendedStyle)))
	{
		DEBUG_PRINTF("Unable to calculate game window frame: %lu\n", GetLastError());
		return false;
	}
	const LONG frameWidth = frameBounds.right - frameBounds.left;
	const LONG frameHeight = frameBounds.bottom - frameBounds.top;
	const LONG maxClientWidth = workWidth - frameWidth;
	const LONG maxClientHeight = workHeight - frameHeight;
	if (maxClientWidth <= 0 || maxClientHeight <= 0)
	{
		DEBUG_PRINTF("Monitor work area cannot fit a decorated game window\n");
		return false;
	}

	// Fixed mode dimensions refer to the client area. If the decorated outer
	// window would exceed the work area, use the documented default scale rather
	// than silently shrinking the user's requested fixed dimensions.
	LONG clientWidth = 0;
	LONG clientHeight = 0;
	if (configSettings.windowSizingMode == lcu_config::WindowSizingMode::Fixed &&
		configSettings.windowWidth > 0 && configSettings.windowHeight > 0 &&
		configSettings.windowWidth <= maxClientWidth &&
		configSettings.windowHeight <= maxClientHeight)
	{
		clientWidth = configSettings.windowWidth;
		clientHeight = configSettings.windowHeight;
	}
	else
	{
		float windowScale = configSettings.windowScale;
		if (configSettings.windowSizingMode == lcu_config::WindowSizingMode::Fixed)
		{
			DEBUG_PRINTF("Fixed window dimensions do not fit this monitor; using default scale\n");
			configSettings.windowSizingMode = lcu_config::WindowSizingMode::Scale;
			windowScale = lcu_config::DefaultWindowScale;
			configSettings.windowScale = windowScale;
			configSettings.windowWidth = lcu_config::DefaultWindowWidth;
			configSettings.windowHeight = lcu_config::DefaultWindowHeight;
		}

		// Only scale mode needs the current rendering aspect ratio. Fixed mode
		// uses the client's explicitly requested dimensions without adjustment.
		RECT currentClientRect = {};
		if (!GetClientRect(window, &currentClientRect))
		{
			DEBUG_PRINTF("Unable to read game client bounds: %lu\n", GetLastError());
			return false;
		}
		const LONG currentClientWidth = currentClientRect.right - currentClientRect.left;
		const LONG currentClientHeight = currentClientRect.bottom - currentClientRect.top;
		if (currentClientWidth <= 0 || currentClientHeight <= 0)
		{
			DEBUG_PRINTF("Invalid game client dimensions\n");
			return false;
		}
		const double gameAspectRatio = static_cast<double>(currentClientWidth) / currentClientHeight;

		// Fit the game's aspect ratio first, then scale both dimensions uniformly
		// and center the result in the available work area.
		const double fittingWidth = (std::min)(
			static_cast<double>(maxClientWidth),
			static_cast<double>(maxClientHeight) * gameAspectRatio);
		const double fittingHeight = fittingWidth / gameAspectRatio;
		clientWidth = (std::max)(1L,
			static_cast<LONG>(std::lround(fittingWidth * windowScale)));
		clientHeight = (std::max)(1L,
			static_cast<LONG>(std::lround(fittingHeight * windowScale)));
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
	if (windowWidth > workWidth || windowHeight > workHeight)
	{
		DEBUG_PRINTF("Calculated game window does not fit the monitor work area\n");
		return false;
	}
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

	const bool isWindowToggleKey = wParam == configSettings.windowToggleKey;
	if ((Msg == WM_KEYDOWN || Msg == WM_SYSKEYDOWN) && isWindowToggleKey)
	{
		if ((static_cast<std::uintptr_t>(lParam) & (1ULL << 30)) == 0)
		{
			ToggleWindowMode(hWnd);
		}
	}
	if ((Msg == WM_KEYUP || Msg == WM_SYSKEYUP) && isWindowToggleKey)
	{
		return 0;
	}

	switch (Msg)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (mouse_camera::ProcessToggleKey(desiredState, wParam, lParam,
			configSettings.mouseToggleKey) ==
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
	if (isWindowToggleKey)
	{
		// Process both actions if a user intentionally binds the two toggles to
		// the same key, while still keeping the window hotkey out of the game.
		return 0;
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
		// Keep keyboard adjustment deliberately finer than the original 0.005 step.
		configSettings.sensitivity += lcu_config::SensitivityAdjustmentStep;
	}
	if (cameraModeAllowed && GetAsyncKeyState(VK_END)) {
		configSettings.sensitivity -= lcu_config::SensitivityAdjustmentStep;
		if (configSettings.sensitivity < lcu_config::MinimumSensitivity)
		{
			configSettings.sensitivity = lcu_config::MinimumSensitivity;
		}
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
		rotation = mouse_camera::AdvanceRotation(
			rotation, horizontalMouseDelta, configSettings.sensitivity);
	}
	auto rotatedCam = RotateCam(Vector3(playerPos[0] - distance, playerPos[2] - distance, 0), Vector3(playerPos[0], playerPos[2], 0), rotation);

	camPos[0] = rotatedCam.x;
	camPos[2] = rotatedCam.y;

	//DEBUG_PRINTF("ROTATION %.6f %.6f\n", rotatedCam.x, rotatedCam.y);

	fpUpdateCam(a1, camPos, playerPos, a4, a5, a6, a7, a8, a9);
}



static constexpr char ConfigHeaderTemplate[] =
	"; LEGO City Undercover Mouse Plus settings.\n"
	";\n"
	"; Keyboard keys can be \"Friendly\" names (like F8), keys (like w), or decimal/hex virtual-key codes (like 0x77)\n"
	"; Keyboard key used to enable or disable mouse-camera control. Examples: F8, HOME, END, ESC, PAGEUP, K, 9, 0x77.\n";



static constexpr char MouseConfigTemplate[] =
	"[mouse]\n"
	"toggle=F8\n"
	"; Mouse-camera sensitivity must be greater than 0. Use Home/End to increase/decrease the sensitivity by 0.001.\n"
	"sensitivity=0.003\n";



static constexpr char WindowConfigTemplate[] =
	"[window]\n"
	"toggle=F9\n"
	"; Window sizing mode: scale or fixed.\n"
	"mode=scale\n"
	"; Scale fits the game aspect ratio\n"
	"window_scale=0.8\n"
	"; Fixed uses window_width and window_height.\n"
	"window_width=1280\n"
	"window_height=720\n";



static bool AppendMissingConfigSections(const mINI::INIStructure& ini)
{
	// Legacy files keep all their existing text; append documented sections only
	// when that whole section is absent, then let mINI parse and update it lazily.
	const bool needsMouseSection = !ini.has("mouse");
	const bool needsWindowSection = !ini.has("window");
	if (!needsMouseSection && !needsWindowSection)
	{
		return true;
	}

	std::ofstream configFile("lcu.ini", std::ios::out | std::ios::app | std::ios::binary);
	if (!configFile.is_open())
	{
		DEBUG_PRINTF("Unable to append documented settings to lcu.ini\n");
		return false;
	}

	configFile << "\n";
	if (needsMouseSection)
	{
		configFile << ConfigHeaderTemplate << "\n" << MouseConfigTemplate;
	}
	if (needsWindowSection)
	{
		configFile << "\n" << WindowConfigTemplate;
	}
	configFile.flush();
	if (!configFile.good())
	{
		DEBUG_PRINTF("Unable to append documented settings to lcu.ini\n");
		return false;
	}
	return true;
}



static bool EnsureConfigFile()
{
	// Seed missing files with the fully documented template. Existing files are
	// never replaced here; mINI's lazy writer updates them while retaining comments.
	const DWORD existingAttributes = GetFileAttributesA("lcu.ini");
	std::ifstream existingFile("lcu.ini", std::ios::in | std::ios::binary);
	if (existingFile.good() && existingFile.peek() != std::ifstream::traits_type::eof())
	{
		return true;
	}
	if (existingAttributes != INVALID_FILE_ATTRIBUTES &&
		(!existingFile.is_open() || existingFile.bad()))
	{
		DEBUG_PRINTF("Unable to inspect existing lcu.ini; leaving it untouched\n");
		return false;
	}
	if (existingAttributes != INVALID_FILE_ATTRIBUTES &&
		(existingAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		DEBUG_PRINTF("lcu.ini refers to a directory; leaving it untouched\n");
		return false;
	}
	existingFile.close();

	std::ofstream newFile("lcu.ini", std::ios::out | std::ios::binary);
	if (!newFile.is_open())
	{
		DEBUG_PRINTF("Unable to create lcu.ini: %lu\n", GetLastError());
		return false;
	}

	newFile << ConfigHeaderTemplate << "\n" << MouseConfigTemplate << "\n" <<
		WindowConfigTemplate;
	newFile.flush();
	if (!newFile.good())
	{
		DEBUG_PRINTF("Unable to write default lcu.ini template\n");
		return false;
	}
	return true;
}



static std::string FormatSensitivity(float value)
{
	// Find the shortest significant decimal that round-trips to the same float,
	// keeping ordinary values such as 0.16 readable without losing precision.
	for (int precision = 1; precision <= std::numeric_limits<float>::max_digits10; ++precision)
	{
		std::ostringstream formatted;
		formatted << std::setprecision(precision) << value;
		const std::string candidate = formatted.str();
		try
		{
			std::size_t parsedCharacters = 0;
			if (std::stof(candidate, &parsedCharacters) == value &&
				parsedCharacters == candidate.size())
			{
				return candidate;
			}
		}
		catch (const std::exception&)
		{
			// The stream output is expected to be parseable; try the next precision
			// rather than writing a potentially lossy representation.
		}
	}

	// max_digits10 guarantees a round-trip fallback for finite float values.
	std::ostringstream formatted;
	formatted << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
	return formatted.str();
}



static std::string ConfiguredKeyOrDefault(const mINI::INIStructure& ini,
	const char* sectionName, const char* settingName, const char* defaultValue)
{
	if (ini.has(sectionName))
	{
		const auto section = ini.get(sectionName);
		if (section.has(settingName))
		{
			const std::string configuredValue = section.get(settingName);
			unsigned short ignoredKey = 0;
			if (lcu_config::ParseKeyboardKey(configuredValue, ignoredKey))
			{
				return configuredValue;
			}
		}
	}
	return defaultValue;
}



static bool IsPositiveConfigDimension(
	const mINI::INIMap<std::string>& section, const char* name)
{
	if (!section.has(name))
	{
		return false;
	}

	try
	{
		const std::string value = section.get(name);
		std::size_t parsedCharacters = 0;
		const long parsed = std::stol(value, &parsedCharacters, 10);
		return parsedCharacters == value.size() && parsed > 0 &&
			parsed <= (std::numeric_limits<int>::max)();
	}
	catch (const std::exception&)
	{
		return false;
	}
}



static bool ReadConfigFile(mINI::INIFile& file, mINI::INIStructure& ini)
{
	if (!EnsureConfigFile())
	{
		return false;
	}
	if (!file.read(ini))
	{
		DEBUG_PRINTF("Unable to read lcu.ini\n");
		return false;
	}
	if (!AppendMissingConfigSections(ini))
	{
		return false;
	}
	// Reading again picks up any fully commented defaults appended to a legacy
	// file, while mINI's later lazy write continues preserving every old comment.
	if (!file.read(ini))
	{
		DEBUG_PRINTF("Unable to reread lcu.ini after adding documented settings\n");
		return false;
	}
	return true;
}



static bool WriteConfigFile(mINI::INIFile& file, mINI::INIStructure& ini)
{
	// Remember which ignored fields the user supplied before default insertion;
	// this lets scale mode leave fixed dimensions alone unless fixing invalid data.
	const auto originalWindow = ini.get("window");
	std::string requestedMode = originalWindow.get("mode");
	std::transform(requestedMode.begin(), requestedMode.end(), requestedMode.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	const bool requestedFixed = requestedMode == "fixed";

	// Keep accepted user spellings, but normalize invalid or absent bindings to
	// their documented keyboard defaults before the settings are written.
	ini["mouse"]["toggle"] = ConfiguredKeyOrDefault(ini, "mouse", "toggle", "F8");
	ini["mouse"]["sensitivity"] = FormatSensitivity(configSettings.sensitivity);
	ini["window"]["toggle"] = ConfiguredKeyOrDefault(ini, "window", "toggle", "F9");
	ini["window"]["mode"] = configSettings.windowSizingMode ==
		lcu_config::WindowSizingMode::Fixed ? "fixed" : "scale";
	if (configSettings.windowSizingMode == lcu_config::WindowSizingMode::Scale ||
		!originalWindow.has("window_scale"))
	{
		ini["window"]["window_scale"] = FormatSensitivity(configSettings.windowScale);
	}

	// Scaled layouts ignore fixed dimensions, so preserve existing values unless
	// a fixed layout was invalid and explicitly fell back to the default settings.
	if (configSettings.windowSizingMode == lcu_config::WindowSizingMode::Fixed)
	{
		ini["window"]["window_width"] = std::to_string(configSettings.windowWidth);
		ini["window"]["window_height"] = std::to_string(configSettings.windowHeight);
	}
	else if (requestedFixed)
	{
		ini["window"]["window_width"] = std::to_string(lcu_config::DefaultWindowWidth);
		ini["window"]["window_height"] = std::to_string(lcu_config::DefaultWindowHeight);
	}
	else
	{
		if (!IsPositiveConfigDimension(originalWindow, "window_width"))
		{
			ini["window"]["window_width"] =
				std::to_string(lcu_config::DefaultWindowWidth);
		}
		if (!IsPositiveConfigDimension(originalWindow, "window_height"))
		{
			ini["window"]["window_height"] =
				std::to_string(lcu_config::DefaultWindowHeight);
		}
	}

	if (!file.write(ini))
	{
		DEBUG_PRINTF("Unable to update lcu.ini while preserving its comments\n");
		return false;
	}
	return true;
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

			if (ReadConfigFile(file, ini))
			{
				// Load only the new mouse/window settings. The retired [settings]
				// hotkey and sensitivity entries are deliberately ignored.
				configSettings = lcu_config::LoadSettings(ini);

				// These separate game-input bindings are retained for existing users;
				// they are not accepted as mouse-camera or window-mode toggle keys.
				if (ini.has("settings"))
				{
					const auto collection = ini.get("settings");
					if (collection.has("leftclick"))
					{
						leftClickKey = static_cast<unsigned short>(
							std::stoi(collection.get("leftclick"), 0, 16));
					}
					if (collection.has("rightclick"))
					{
						rightClickKey = static_cast<unsigned short>(
							std::stoi(collection.get("rightclick"), 0, 16));
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

			// Home/End sensitivity changes are saved using the new [mouse] key;
			// mINI's lazy write path retains existing explanatory comments/formatting.
			if (ReadConfigFile(file, ini))
			{
				WriteConfigFile(file, ini);
			}
		}
		catch (std::exception& ex)
		{
			DEBUG_PRINTF("Error %s", ex.what());
		}

		break;
	}

	return TRUE;
}
