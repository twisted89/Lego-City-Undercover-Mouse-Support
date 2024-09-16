#include <windows.h>
#include <tchar.h>
#include <stdint.h>
#include <MinHook.h>
#include <stdio.h>
#include <math.h>
#include "vector3.h"
#include "Proxy.h"
#include "RawInput.h"
#include "ini.h"

#define DEBUG_PRINTF(...) {char cad[512]; sprintf(cad, __VA_ARGS__);  OutputDebugStringA(cad);}

LRESULT CALLBACK WndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

struct ButtonEvent
{
	unsigned short vkey;
	bool pressed;
};

typedef int64_t(__fastcall* UPDATECAM)(__int64, float*, float*, float, int, int, char, char, __int64);
UPDATECAM fpUpdateCam = nullptr;

typedef int64_t(__fastcall* FORCECAMDIRECTION)(__int64, __int64);
FORCECAMDIRECTION fpForceCamDir = nullptr;

typedef int64_t(__fastcall* KEYPRESSFUNC)(int);
KEYPRESSFUNC fpKeyPress = nullptr;
unsigned char* keyStates = nullptr;
unsigned char* keyCounter = nullptr;

HWND hwnd = NULL;
WNDPROC fpWndProc = nullptr;

float sensitivity = 1, distance = 1;
int x = 0, y = 0;
float rotation = 0;
bool initialised = false;
bool capture = false;
unsigned short leftClickKey = 0x51, rightClickKey = 0x45;

void OnMouseInput(int mx, int my)
{
	x += mx;
	y += my;
}

void OnMouseScroll(bool up)
{
	distance += up ? -0.1f : 0.1f;
}

void SendButton(unsigned short key, bool down)
{
	//DEBUG_PRINTF("Sending key %d %s\n", key, down ? "DOWN" : "UP");
	INPUT input_config = {};
	input_config.type = INPUT_KEYBOARD;
	input_config.ki.wScan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
	input_config.ki.dwFlags = (KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP));
	SendInput(1, &input_config, sizeof(input_config));
}

void OnMouseButton(unsigned short buttonFlags)
{
	if (buttonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
		SendButton(leftClickKey, true );
	}
	if (buttonFlags & RI_MOUSE_LEFT_BUTTON_UP) {
		SendButton(leftClickKey, false );
	}
	if (buttonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
		SendButton(rightClickKey, true );
	}
	if (buttonFlags & RI_MOUSE_RIGHT_BUTTON_UP) {
		SendButton(rightClickKey, false );
	}
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

RawInput input = RawInput(OnMouseInput, OnMouseScroll, OnMouseButton);

LRESULT CALLBACK WndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
	hwnd = hWnd;
	
	switch (Msg)
	{
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE)
			capture = false;
		else // WA_ACTIVE or WA_CLICKACTIVE 
			capture = true;
		break;
	}
	return fpWndProc(hWnd, Msg, wParam, lParam);
}

int64_t __fastcall UpdateCamDirection(__int64 a1, __int64 a2)
{
	*reinterpret_cast<int*>(a1 + 0x32D4) = 0;
	*reinterpret_cast<char*>(a1 + 0x32D8) = 1;

	return fpForceCamDir(a1, a2);
}

int64_t __fastcall UpdateCamDetour(__int64 a1, float* camPos, float* playerPos , float a4, int a5, int a6, char a7, char a8, __int64 a9)
{
	if (GetAsyncKeyState(VK_HOME)) {
		sensitivity += 0.005f;
	}
	if (GetAsyncKeyState(VK_END)) {
		sensitivity -= 0.005f;
		if (sensitivity <= 0)
			sensitivity = 0.01f;
	}

	if (!initialised)
		initialised = input.Init();

	if (initialised)
	{
		input.Update(capture);

		rotation = static_cast<float>(static_cast<int>((rotation + (-x * sensitivity))) % 360);
		auto rotatedCam = RotateCam(Vector3(playerPos[0] - distance, playerPos[2] - distance, 0), Vector3(playerPos[0], playerPos[2], 0), rotation);
		
		camPos[0] = rotatedCam.x;
		camPos[2] = rotatedCam.y;

		//DEBUG_PRINTF("ROTATION %.6f %.6f\n", rotatedCam.x, rotatedCam.y);

		x = 0;
		y = 0;
	}

	return fpUpdateCam(a1, camPos, playerPos, a4, a5, a6, a7, a8, a9);
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
		GetModuleFileName(GetModuleHandle(nullptr), modulePath, MAX_PATH);

		const TCHAR* dllPath = _T("proxy\\steam_api64.dll");

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

		auto cameraUpdateFP = reinterpret_cast<LPVOID>(moduleBase + 0x991500);
		auto cameraUpdateTestFP = reinterpret_cast<LPVOID>(moduleBase + 0x8ED0A0);
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

		if (MH_CreateHook(cameraUpdateTestFP, &UpdateCamDirection,
			reinterpret_cast<LPVOID*>(&fpForceCamDir)) != MH_OK)
		{
			return FALSE;
		}

		if (MH_EnableHook(cameraUpdateTestFP) != MH_OK)
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
			sprintf(leftclickHex, "%X", leftClickKey);
			sprintf(rightclickHex, "%X", rightClickKey);

			ini["settings"]["sensitivity"] = std::to_string(sensitivity);

			ini["settings"]["leftclick"] = "0x" + std::string(leftclickHex);
			ini["settings"]["rightclick"] = "0x" +  std::string(rightclickHex);

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
