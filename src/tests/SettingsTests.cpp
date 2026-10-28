#include "../config/Settings.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace
{
	int failures = 0;

	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	void CheckNear(float actual, float expected, const char* message)
	{
		Check(std::fabs(actual - expected) < 0.0001f, message);
	}
}

int main()
{
	using namespace lcu_config;

	unsigned short key = 0;
	Check(ParseKeyboardKey("F9", key) && key == 0x78, "F9 parses as a friendly name");
	Check(ParseKeyboardKey("page up", key) && key == 0x21, "friendly key names ignore case and spaces");
	Check(ParseKeyboardKey("Home", key) && key == 0x24, "HOME parses as a friendly name");
	Check(ParseKeyboardKey("K", key) && key == 'K', "single letter binds to that keyboard character");
	Check(ParseKeyboardKey("9", key) && key == '9', "single digit binds to the digit key");
	Check(ParseKeyboardKey("0", key) && key == '0', "single zero character binds to the zero key");
	Check(ParseKeyboardKey("0x78", key) && key == 0x78, "hexadecimal virtual-key codes parse");
	Check(ParseKeyboardKey("0X78", key) && key == 0x78, "hexadecimal prefixes are case-insensitive");
	Check(ParseKeyboardKey("120", key) && key == 120, "decimal virtual-key codes parse");
	Check(ParseKeyboardKey("-", key) && key == 0xBD, "single punctuation key binds correctly");
	Check(ParseKeyboardKey("!", key) && key == '1', "shifted punctuation maps to its keyboard key");
	Check(!ParseKeyboardKey("0x01", key), "numeric left mouse button is rejected");
	Check(!ParseKeyboardKey("0x02", key), "numeric right mouse button is rejected");
	Check(!ParseKeyboardKey("0x04", key), "numeric middle mouse button is rejected");
	Check(!ParseKeyboardKey("0x05", key), "numeric first extra mouse button is rejected");
	Check(!ParseKeyboardKey("0x06", key), "numeric second extra mouse button is rejected");
	Check(!ParseKeyboardKey("0x100", key), "virtual-key values above one byte are rejected");
	Check(!ParseKeyboardKey("0x00", key), "zero virtual-key code is not accepted as a toggle key");
	Check(!ParseKeyboardKey("F25", key), "function keys above F24 are rejected");

	mINI::INIStructure ini;
	Settings settings = LoadSettings(ini);
	Check(settings.mouseToggleKey == 0x77, "default mouse toggle is F8");
	CheckNear(settings.sensitivity, 0.003f, "default sensitivity matches lcu.ini");
	Check(settings.windowToggleKey == 0x78, "default window toggle is F9");
	Check(settings.windowSizingMode == WindowSizingMode::Scale, "default window mode is scale");
	CheckNear(settings.windowScale, 0.80f, "default window scale is 80 percent");
	Check(settings.windowWidth == 1280 && settings.windowHeight == 720,
		"default fixed client dimensions are 1280 by 720");
	CheckNear(SensitivityAdjustmentStep, 0.001f, "Home/End use fine sensitivity adjustments");
	CheckNear(MinimumSensitivity, 0.001f, "Home/End retain a small positive minimum");

	ini["settings"]["togglekey"] = "0x73";
	ini["settings"]["sensitivity"] = "0.9";
	settings = LoadSettings(ini);
	Check(settings.mouseToggleKey == 0x77, "retired togglekey setting is ignored");
	CheckNear(settings.sensitivity, 0.003f, "retired sensitivity location is ignored");

	ini["MOUSE"]["TOGGLE"] = "home";
	ini["mouse"]["sensitivity"] = "1e30";
	ini["window"]["toggle"] = "END";
	ini["window"]["mode"] = "ScAlE";
	ini["window"]["window_scale"] = "0.65";
	ini["window"]["window_width"] = "1600";
	ini["window"]["window_height"] = "900";
	settings = LoadSettings(ini);
	Check(settings.mouseToggleKey == 0x24, "mouse hotkey uses case-insensitive friendly names");
	CheckNear(settings.sensitivity, 1e30f, "sensitivity has no arbitrary upper bound");
	Check(settings.windowToggleKey == 0x23, "window hotkey uses case-insensitive friendly names");
	Check(settings.windowSizingMode == WindowSizingMode::Scale, "scale mode name is case-insensitive");
	CheckNear(settings.windowScale, 0.65f, "scale mode accepts a valid fraction");
	Check(settings.windowWidth == 1280 && settings.windowHeight == 720,
		"scale mode retains sensible fixed-mode defaults");

	ini["mouse"]["toggle"] = "0x01";
	ini["mouse"]["sensitivity"] = "nan";
	ini["window"]["toggle"] = "0x06";
	ini["window"]["window_scale"] = "1.0";
	settings = LoadSettings(ini);
	Check(settings.mouseToggleKey == 0x77, "invalid mouse toggle falls back to F8");
	CheckNear(settings.sensitivity, 0.003f, "non-finite sensitivity falls back to default");
	Check(settings.windowToggleKey == 0x78, "invalid window toggle falls back to F9");
	CheckNear(settings.windowScale, 0.80f, "out-of-range scale falls back to default");

	const char* invalidSensitivities[] = { "0", "-0.1", "inf", "not-a-number", "0.16junk" };
	for (const char* value : invalidSensitivities)
	{
		ini["mouse"]["sensitivity"] = value;
		settings = LoadSettings(ini);
		CheckNear(settings.sensitivity, 0.003f, "invalid sensitivity falls back to lcu.ini default");
	}
	ini["mouse"]["sensitivity"] = "0.25";
	settings = LoadSettings(ini);
	CheckNear(settings.sensitivity, 0.25f, "valid positive sensitivity loads");

	ini["window"]["mode"] = "fixed";
	ini["window"]["window_width"] = "1280";
	ini["window"]["window_height"] = "720";
	settings = LoadSettings(ini);
	Check(settings.windowSizingMode == WindowSizingMode::Fixed, "fixed mode name is case-insensitive");
	Check(settings.windowWidth == 1280 && settings.windowHeight == 720,
		"fixed mode loads positive client-area dimensions");

	ini["window"]["window_width"] = "0";
	settings = LoadSettings(ini);
	Check(settings.windowSizingMode == WindowSizingMode::Scale,
		"invalid fixed dimensions fall back to scale mode");
	CheckNear(settings.windowScale, 0.80f,
		"invalid fixed dimensions use the default scale, not the configured scale");
	Check(settings.windowWidth == 1280 && settings.windowHeight == 720,
		"invalid fixed dimensions leave sensible defaults available for editing");

	ini["window"]["mode"] = "unknown";
	ini["window"]["window_width"] = "1280";
	settings = LoadSettings(ini);
	Check(settings.windowSizingMode == WindowSizingMode::Scale,
		"unknown sizing mode falls back to scale mode");
	CheckNear(settings.windowScale, 0.80f, "unknown sizing mode uses the default scale");

	// Exercise the same lazy-write path used by the DLL so updates cannot regress
	// into replacing a user's annotated configuration with uncommented output.
	const char* commentTestPath = "SettingsTests.ini";
	{
		std::ofstream testConfig(commentTestPath, std::ios::out | std::ios::binary);
		testConfig << "; preserve this file-level explanation\n"
			"[mouse]\n"
			"; preserve this setting explanation\n"
			"toggle=F8\n"
			"sensitivity=0.16\n";
	}
	mINI::INIFile commentTestFile(commentTestPath);
	mINI::INIStructure commentTestIni;
	Check(commentTestFile.read(commentTestIni), "comment-retention fixture can be read");
	commentTestIni["mouse"]["sensitivity"] = "0.165";
	Check(commentTestFile.write(commentTestIni), "comment-retention fixture can be updated");
	{
		std::ifstream updatedConfig(commentTestPath, std::ios::in | std::ios::binary);
		const std::string updatedContents(
			(std::istreambuf_iterator<char>(updatedConfig)), std::istreambuf_iterator<char>());
		Check(updatedContents.find("; preserve this file-level explanation") != std::string::npos,
			"lazy INI updates preserve file-level comments");
		Check(updatedContents.find("; preserve this setting explanation") != std::string::npos,
			"lazy INI updates preserve setting comments");
		Check(updatedContents.find("sensitivity=0.165") != std::string::npos,
			"lazy INI updates persist a changed sensitivity value");
	}
	std::remove(commentTestPath);

	if (failures != 0)
	{
		std::cerr << failures << " test(s) failed\n";
		return 1;
	}

	std::cout << "SettingsTests passed\n";
	return 0;
}
