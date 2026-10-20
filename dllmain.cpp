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
		// Loading and fast travel stop this hook long enough to invalidate accumulated
		// input and, potentially, the thread-owned raw-input message window.
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

			// Update(true) also recreates the raw-input queue if loading replaced the
			// camera thread that owned its message-only window.
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
		rotation = static_cast<float>(static_cast<int>((rotation + (-horizontalMouseDelta * sensitivity))) % 360);
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
