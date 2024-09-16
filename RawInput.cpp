#include "RawInput.h"
#include <stdio.h>

#define HID_USAGE_PAGE_GENERIC 1
#define HID_USAGE_GENERIC_MOUSE 2

static const wchar_t* class_name = L"RawInputCatcher";
static const wchar_t* win_name = L"RawInputMsgWindow";

RawInput* RawInput::ctx = nullptr;

RawInput::RawInput(std::function<void(int x, int y)> moveCallback, 
	std::function<void(bool up)> scrollCallback,
	std::function<void(unsigned short button)> buttonCallback) :
	m_moveCallback(moveCallback),
	m_scrollCallback(scrollCallback),
	m_buttonCallback(buttonCallback),
	m_hdevinfo(INVALID_HANDLE_VALUE),
	m_devinfos(nullptr),
	nb_devinfos(0),
	m_mice(nullptr),
	nb_mice(0),
	m_registered(false),
	m_isWow64(false),
	m_hwnd(0),
	m_class_atom(0)
{
	ctx = this;
}

bool RawInput::get_devinfos() {

	const int flags = DIGCF_ALLCLASSES | DIGCF_PRESENT;
	m_hdevinfo = SetupDiGetClassDevs(NULL, NULL, NULL, flags);
	if (m_hdevinfo == INVALID_HANDLE_VALUE) {
		printf("SetupDiGetClassDevs failed");
		return false;
	}

	int i = 0;
	while (1) {
		SP_DEVINFO_DATA data = {  };
		data.cbSize = sizeof(SP_DEVINFO_DATA);
		BOOL result = SetupDiEnumDeviceInfo(m_hdevinfo, i++, &data);
		if (result == TRUE) {
			unsigned long bufsize;
			result = SetupDiGetDeviceInstanceId(m_hdevinfo, &data, nullptr, 0, &bufsize);
			if (result == FALSE && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
				char* buf = (char*)malloc(bufsize);
				if (buf == NULL) {
					printf("malloc failed");
					continue;
				}
				result = SetupDiGetDeviceInstanceIdA(m_hdevinfo, &data, buf, bufsize, NULL);
				if (result == TRUE) {
					deviceInfo* ptr = reinterpret_cast<deviceInfo*>(realloc(m_devinfos, (nb_devinfos + 1) * sizeof(deviceInfo)));
					if (ptr == NULL) {
						printf("malloc failed");
						free(buf);
						continue;
					}
					m_devinfos = ptr;
					m_devinfos[nb_devinfos].instanceId = buf;
					m_devinfos[nb_devinfos].data = data;
					++nb_devinfos;
				}
				else {
					printf("SetupDiGetDeviceInstanceId failed");
					free(buf);
				}
			}
			else {
				printf("SetupDiGetDeviceInstanceId failed");
			}
		}
		else if (GetLastError() == ERROR_NO_MORE_ITEMS) {
			break;
		}
		else {
			printf("SetupDiEnumDeviceInfo failed");
		}
	}

	return true;
}

RawInput::~RawInput()
{
	rawinput_quit();
}

void RawInput::free_dev_info() {
	if (m_hdevinfo != INVALID_HANDLE_VALUE) {
		SetupDiDestroyDeviceInfoList(m_hdevinfo);
	}
	unsigned int i;
	for (i = 0; i < nb_devinfos; ++i) {
		free(m_devinfos[i].instanceId);
	}
	free(m_devinfos);
	m_devinfos = nullptr;
	nb_devinfos = 0;
}


SP_DEVINFO_DATA* RawInput::get_devinfo_data(const char* instanceId) {

	unsigned int i;
	for (i = 0; i < nb_devinfos; ++i) {
		if (_stricmp(instanceId, m_devinfos[i].instanceId) == 0) {
			return &m_devinfos[i].data;
		}
	}
	return NULL;
}

char* RawInput::utf16le_to_utf8(const unsigned char* inbuf)
{
	char* outbuf = NULL;
	int outsize = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)inbuf, -1, NULL, 0, NULL, NULL);
	if (outsize != 0) {
		outbuf = (char*)malloc(outsize);
		if (outbuf != NULL) {
			int res = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)inbuf, -1, outbuf, outsize, NULL, NULL);
			if (res == 0) {
				printf("WideCharToMultiByte failed");
				free(outbuf);
				outbuf = NULL;
			}
		}
	}
	else {
		printf("WideCharToMultiByte failed");
	}

	return outbuf;
}

void RawInput::rawinput_handler(const RAWINPUT* raw, UINT align) {

	unsigned int device;
	const RAWINPUTHEADER* header = &raw->header;
	const RAWMOUSE* mouse = (RAWMOUSE*)(&raw->data.mouse + align);

	if (raw->header.dwType == RIM_TYPEMOUSE) {

		for (device = 0; device < nb_mice; ++device) {
			if (m_mice[device].handle == header->hDevice) {
				break;
			}
		}

		if (device == nb_mice) {
			return;
		}

		if (mouse->usFlags == MOUSE_MOVE_RELATIVE) {
			m_moveCallback(mouse->lLastX, mouse->lLastY);
		}

		if (mouse->usButtonFlags & RI_MOUSE_WHEEL) {
			if (mouse->usButtonData != 0) {
				m_scrollCallback(((SHORT)mouse->usButtonData) > 0 ? true : false);
			}
		}
		
		m_buttonCallback(mouse->usButtonFlags);
	}
	else {
		return;
	}
}

void RawInput::wminput_handler(WPARAM wParam, LPARAM lParam)
{
	(void)(wParam);
	UINT dwSize = 0;

	GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));

	if (dwSize == 0) {
		return;
	}

	LPBYTE lpb = (LPBYTE)malloc(dwSize);

	if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
		return;
	}

	if(ctx)
		ctx->rawinput_handler((RAWINPUT*)lpb, 0);

	free(lpb);
}

LRESULT CALLBACK RawInput::RawWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {

	if (Msg == WM_DESTROY) {
		return 0;
	}

	if (Msg == WM_INPUT) {
		wminput_handler(wParam, lParam);
		return true;
	}

	return DefWindowProc(hWnd, Msg, wParam, lParam);
}

char* RawInput::get_dev_name_by_instance(const char* devinstance) {

	SP_DEVINFO_DATA* devdata = get_devinfo_data(devinstance);
	if (devdata != NULL) {
		DWORD size;
		BOOL result = SetupDiGetDeviceRegistryPropertyW(m_hdevinfo, devdata, SPDRP_DEVICEDESC, NULL, NULL, 0, &size);
		if (result == FALSE && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
			unsigned char* desc = (unsigned char*)malloc(size);
			result = SetupDiGetDeviceRegistryPropertyW(m_hdevinfo, devdata, SPDRP_DEVICEDESC, NULL, desc, size, NULL);
			if (result == FALSE) {
				printf("SetupDiGetDeviceRegistryProperty failed");
				free(desc);
			}
			else {
				char* devName = utf16le_to_utf8(desc);
				free(desc);
				return devName;
			}
		}
		else {
			printf("SetupDiGetDeviceRegistryProperty failed");
		}
	}

	return NULL;
}

void RawInput::init_device(const RAWINPUTDEVICELIST* dev) {

	if (dev->dwType != RIM_TYPEMOUSE && dev->dwType != RIM_TYPEKEYBOARD) {
		return;
	}

	UINT count = 0;
	if (GetRawInputDeviceInfoA(dev->hDevice, RIDI_DEVICENAME, NULL, &count) == (UINT)-1) {
		printf("GetRawInputDeviceInfo failed");
		return;
	}

	char* abuf = (char*)malloc(count + 1);

	if (!abuf) {
		printf("init_device out of memory!");
		return;
	}

	memset(abuf, 0, count + 1);

	char* buf = abuf;

	if (GetRawInputDeviceInfoA(dev->hDevice, RIDI_DEVICENAME, buf, &count) == (UINT)-1) {
		printf("GetRawInputDeviceInfo failed");
		free(abuf);
		return;
	}

	// skip remote desktop devices
	if (strstr(buf, "Root#RDP_")) {
		free(abuf);
		return;
	}

	// XP starts these strings with "\\??\\" ... Vista does "\\\\?\\".  :/
	while ((*buf == '?') || (*buf == '\\')) {
		buf++;
		count--;
	}

	// get the device instance id
	char* ptr;
	for (ptr = buf; *ptr; ptr++) {
		if (*ptr == '#') {
			*ptr = '\\'; // convert '#' to '\\'
		}
		else if (*ptr == '{') { // GUID part
			if (*(ptr - 1) == '\\') {
				ptr--;
			}
			break;
		}
	}
	*ptr = '\0';

	if (dev->dwType == RIM_TYPEMOUSE) {
		char* name = get_dev_name_by_instance(buf);
		if (name == NULL) {
			free(abuf);
			return;
		}
		mouseDevice* ptr = reinterpret_cast<mouseDevice*>(realloc(m_mice, (nb_mice + 1) * sizeof(mouseDevice)));
		if (ptr == NULL) {
			printf("realloc failed");
			free(name);
			free(abuf);
			return;
		}
		m_mice = ptr;
		m_mice[nb_mice].name = name;
		m_mice[nb_mice].handle = dev->hDevice;
		nb_mice++;
	}

	free(abuf);
}

void RawInput::cleanup_window() {

	if (m_hwnd) {
		MSG Msg;
		DestroyWindow(m_hwnd);
		while (PeekMessage(&Msg, m_hwnd, 0, 0, PM_REMOVE)) {
			TranslateMessage(&Msg);
			DispatchMessage(&Msg);
		}
		m_hwnd = 0;
	}

	if (m_class_atom) {
		UnregisterClass(class_name, GetModuleHandle(NULL));
		m_class_atom = 0;
	}

	//LockSetForegroundWindow(LSFW_UNLOCK);
}

void RawInput::rawinput_quit() {

	register_raw_input(0);
	cleanup_window();
	for (int i = 0; i < nb_mice; ++i) {
		free(m_mice[i].name);
	}
	free(m_mice);
	m_mice = nullptr;
	nb_mice = 0;
}

bool RawInput::register_raw_input(bool enable) {

	if (m_registered == enable) {
		return false;
	}
	if (enable && m_hwnd == NULL)
	{
		printf("Raw HWND null, unable to register input");
		return false;
	}

	DWORD dwFlags = enable ? (RIDEV_NOLEGACY | RIDEV_EXINPUTSINK) : RIDEV_REMOVE;
	HWND hwndTarget = enable ? m_hwnd : 0;

	RAWINPUTDEVICE rid[] = {
		{
			HID_USAGE_PAGE_GENERIC,
			HID_USAGE_GENERIC_MOUSE,
			dwFlags,
			hwndTarget
		}
	};

	if (RegisterRawInputDevices(rid, sizeof(rid) / sizeof(*rid), sizeof(*rid)) == FALSE) {
		printf("RegisterRawInputDevices failed %d", GetLastError());
		return false;
	}

	m_registered = enable;

	return true;
}

bool RawInput::init_event_queue()
{
	HANDLE hInstance = GetModuleHandle(NULL);

	WNDCLASSEX wce = {  };
	wce.cbSize = sizeof(WNDCLASSEX);
	wce.lpfnWndProc = RawWndProc;
	wce.lpszClassName = class_name;
	wce.hInstance = (HINSTANCE)hInstance;
	m_class_atom = RegisterClassEx(&wce);
	if (m_class_atom == 0)
		return false;

	//create the window at the position of the cursor
	POINT cursor_pos;
	GetCursorPos(&cursor_pos);

	m_hwnd = CreateWindowEx(0, class_name, win_name, /*WS_POPUP*/ 0, cursor_pos.x, cursor_pos.y, 0, 0, HWND_MESSAGE, 0, (HINSTANCE)hInstance, 0);

	if (m_hwnd == NULL) {
		printf("CreateWindow failed %d", GetLastError());
		return false;
	}

	ShowWindow(m_hwnd, SW_HIDE);

	return true;
}

void RawInput::Update(bool capture) {

	register_raw_input(capture);
	MSG Msg;
	while (PeekMessage(&Msg, m_hwnd, 0, 0, PM_REMOVE)) {
		TranslateMessage(&Msg);
		DispatchMessage(&Msg);
	}
}

bool RawInput::Init()
{
	if (!get_devinfos()) {
		return false;
	}

	UINT count = 0;
	UINT result = GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST));
	if (result == (UINT)-1) {
		printf("GetRawInputDeviceList failed");
	}
	else if (count > 0) {
		RAWINPUTDEVICELIST* devlist = (PRAWINPUTDEVICELIST)malloc(count * sizeof(RAWINPUTDEVICELIST));
		if (devlist)
		{
			result = GetRawInputDeviceList(devlist, &count, sizeof(RAWINPUTDEVICELIST));
			if (result != (UINT)-1) {
				unsigned int i;
				for (i = 0; i < result; i++) { // result may be lower than count!
					init_device(&devlist[i]);
				}
			}
			free(devlist);
		}
	}

	free_dev_info();

	if (result == (UINT)-1) {
		rawinput_quit();
		return false;
	}

	if (!init_event_queue()) {
		rawinput_quit();
		return false;
	}

	IsWow64Process(GetCurrentProcess(), &m_isWow64);

	return true;
}
