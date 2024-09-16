#pragma once
#include <functional>
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

	void Update(bool enable);
	bool Init();

private:
	static char* utf16le_to_utf8(const unsigned char* inbuf);
	static void wminput_handler(WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK RawWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
	void rawinput_handler(const RAWINPUT* raw, UINT align);
	bool get_devinfos();
	void free_dev_info();
	SP_DEVINFO_DATA* get_devinfo_data(const char* instanceId);
	char* get_dev_name_by_instance(const char* devinstance);
	void init_device(const RAWINPUTDEVICELIST* dev);
	bool register_raw_input(bool enable);
	void cleanup_window();
	void rawinput_quit();
	bool init_event_queue();

private:
	struct deviceInfo {
		char* instanceId;
		SP_DEVINFO_DATA data;
	};

	struct mouseDevice {
		HANDLE handle;
		char* name;
	};

	static RawInput* ctx;

	std::function<void(int x, int y)> m_moveCallback;
	std::function<void(bool up)> m_scrollCallback;
	std::function<void(unsigned short buttonFlags)> m_buttonCallback;
	HDEVINFO m_hdevinfo;
	deviceInfo *m_devinfos;
	size_t nb_devinfos;
	mouseDevice* m_mice;
	size_t nb_mice;
	bool m_registered;
	HWND m_hwnd;
	ATOM m_class_atom;
	BOOL m_isWow64;
};

