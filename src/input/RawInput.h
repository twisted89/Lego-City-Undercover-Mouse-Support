#pragma once
#include <functional>
#include <mutex>
#include <Windows.h>
#include <Setupapi.h>

class RawInput
{
public:
	RawInput(std::function<void(int x, int y)> callback, 
		std::function<void(bool up)> scrollCallback,
		std::function<void(unsigned short button)> buttonCallback
	);
	~RawInput();

	// Raw input is registered to the game's long-lived window, never to a window
	// owned by the camera worker that calls Update.
	void SetTargetWindow(HWND window);
	// The hooked game WndProc forwards WM_INPUT here, then still calls the original
	// WndProc so the game retains its normal message handling.
	void HandleRawInput(WPARAM wParam, LPARAM lParam);
	void Update(bool enable);
	void DisableCapture();
	bool Init();

private:
	static char* utf16le_to_utf8(const unsigned char* inbuf);
	void rawinput_handler(const RAWINPUT* raw, UINT align);
	bool get_devinfos();
	void free_dev_info();
	SP_DEVINFO_DATA* get_devinfo_data(const char* instanceId);
	char* get_dev_name_by_instance(const char* devinstance);
	void init_device(const RAWINPUTDEVICELIST* dev);
	bool register_raw_input(bool enable);
	void rawinput_quit();

private:
	struct deviceInfo {
		char* instanceId;
		SP_DEVINFO_DATA data;
	};

	struct mouseDevice {
		HANDLE handle;
		char* name;
	};

	std::function<void(int x, int y)> m_moveCallback;
	std::function<void(bool up)> m_scrollCallback;
	std::function<void(unsigned short buttonFlags)> m_buttonCallback;
	HDEVINFO m_hdevinfo;
	deviceInfo *m_devinfos;
	size_t nb_devinfos;
	mouseDevice* m_mice;
	size_t nb_mice;
	bool m_registered;
	HWND m_targetWindow;
	std::mutex m_updateMutex;
	std::mutex m_registrationMutex;
};

