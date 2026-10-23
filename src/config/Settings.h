#pragma once

#include <string>

#include "ini.h"

namespace lcu_config
{
	constexpr unsigned short DefaultMouseToggleKey = 0x77;
	constexpr unsigned short DefaultWindowToggleKey = 0x78;
	constexpr float DefaultSensitivity = 0.003f;
	constexpr float DefaultWindowScale = 0.80f;
	constexpr int DefaultWindowWidth = 1280;
	constexpr int DefaultWindowHeight = 720;
	constexpr float SensitivityAdjustmentStep = 0.001f;
	constexpr float MinimumSensitivity = 0.001f;

	enum class WindowSizingMode
	{
		Scale,
		Fixed
	};

	struct Settings
	{
		unsigned short mouseToggleKey = DefaultMouseToggleKey;
		float sensitivity = DefaultSensitivity;
		unsigned short windowToggleKey = DefaultWindowToggleKey;
		WindowSizingMode windowSizingMode = WindowSizingMode::Scale;
		float windowScale = DefaultWindowScale;
		int windowWidth = DefaultWindowWidth;
		int windowHeight = DefaultWindowHeight;
	};

	bool ParseKeyboardKey(const std::string& value, unsigned short& virtualKey);
	Settings LoadSettings(const mINI::INIStructure& ini);
}
