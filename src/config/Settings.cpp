#include "Settings.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>
#include <string>

namespace lcu_config
{
	namespace
	{
		struct KeyName
		{
			const char* name;
			unsigned short virtualKey;
		};

		constexpr KeyName FriendlyKeyNames[] =
		{
			{ "BACKSPACE", 0x08 }, { "BKSP", 0x08 },
			{ "TAB", 0x09 },
			{ "ENTER", 0x0D }, { "RETURN", 0x0D },
			{ "SHIFT", 0x10 }, { "CTRL", 0x11 }, { "CONTROL", 0x11 },
			{ "ALT", 0x12 }, { "PAUSE", 0x13 },
			{ "CAPSLOCK", 0x14 }, { "ESC", 0x1B }, { "ESCAPE", 0x1B },
			{ "SPACE", 0x20 },
			{ "PAGEUP", 0x21 }, { "PGUP", 0x21 },
			{ "PAGEDOWN", 0x22 }, { "PGDN", 0x22 },
			{ "END", 0x23 }, { "HOME", 0x24 },
			{ "LEFT", 0x25 }, { "UP", 0x26 }, { "RIGHT", 0x27 }, { "DOWN", 0x28 },
			{ "PRINTSCREEN", 0x2C }, { "PRTSC", 0x2C },
			{ "INSERT", 0x2D }, { "INS", 0x2D },
			{ "DELETE", 0x2E }, { "DEL", 0x2E },
			{ "CLEAR", 0x0C }, { "SELECT", 0x29 }, { "HELP", 0x2F },
			{ "APPS", 0x5D }, { "MENU", 0x5D },
			{ "LEFTWIN", 0x5B }, { "LWIN", 0x5B },
			{ "RIGHTWIN", 0x5C }, { "RWIN", 0x5C },
			{ "NUMLOCK", 0x90 }, { "SCROLLLOCK", 0x91 },
			{ "NUMPAD0", 0x60 }, { "NUMPAD1", 0x61 }, { "NUMPAD2", 0x62 },
			{ "NUMPAD3", 0x63 }, { "NUMPAD4", 0x64 }, { "NUMPAD5", 0x65 },
			{ "NUMPAD6", 0x66 }, { "NUMPAD7", 0x67 }, { "NUMPAD8", 0x68 },
			{ "NUMPAD9", 0x69 },
			{ "MULTIPLY", 0x6A }, { "ADD", 0x6B }, { "SUBTRACT", 0x6D },
			{ "DECIMAL", 0x6E }, { "DIVIDE", 0x6F },
			{ "LEFTSHIFT", 0xA0 }, { "RIGHTSHIFT", 0xA1 },
			{ "LEFTCTRL", 0xA2 }, { "RIGHTCTRL", 0xA3 },
			{ "LEFTALT", 0xA4 }, { "RIGHTALT", 0xA5 }
		};

		std::string NormalizeKeyName(const std::string& value)
		{
			std::string normalized;
			for (unsigned char character : value)
			{
				if (std::isspace(character))
				{
					continue;
				}
				normalized.push_back(static_cast<char>(std::toupper(character)));
			}
			return normalized;
		}

		bool IsMouseButtonKey(unsigned long virtualKey)
		{
			// Win32 reserves these virtual-key codes for mouse buttons. Reject them
			// even when supplied numerically so toggle actions remain keyboard-only.
			return virtualKey == 0x01 || virtualKey == 0x02 || virtualKey == 0x04 ||
				virtualKey == 0x05 || virtualKey == 0x06;
		}

		bool ParseNumericKey(const std::string& value, unsigned short& virtualKey)
		{
			if (value.empty())
			{
				return false;
			}

			const int base = value.size() > 2 && value[0] == '0' &&
				(value[1] == 'x' || value[1] == 'X') ? 16 : 10;
			const char* digits = value.c_str();
			if (base == 16)
			{
				digits += 2;
			}

			char* end = nullptr;
			const unsigned long parsed = std::strtoul(digits, &end, base);
			if (end == digits || *end != '\0' || parsed == 0 || parsed > 0xFF ||
				IsMouseButtonKey(parsed))
			{
				return false;
			}

			virtualKey = static_cast<unsigned short>(parsed);
			return true;
		}

		bool ParseSensitivity(const std::string& value, float& sensitivity)
		{
			try
			{
				std::size_t parsedCharacters = 0;
				const float parsed = std::stof(value, &parsedCharacters);
				if (parsedCharacters != value.size() || !std::isfinite(parsed) || parsed <= 0.0f)
				{
					return false;
				}

				sensitivity = parsed;
				return true;
			}
			catch (const std::exception&)
			{
				return false;
			}
		}

		bool ParsePositiveInteger(const std::string& value, int& result)
		{
			try
			{
				std::size_t parsedCharacters = 0;
				const long parsed = std::stol(value, &parsedCharacters, 10);
				if (parsedCharacters != value.size() || parsed <= 0 ||
					parsed > (std::numeric_limits<int>::max)())
				{
					return false;
				}

				result = static_cast<int>(parsed);
				return true;
			}
			catch (const std::exception&)
			{
				return false;
			}
		}

		bool ParseWindowScale(const std::string& value, float& scale)
		{
			try
			{
				std::size_t parsedCharacters = 0;
				const float parsed = std::stof(value, &parsedCharacters);
				if (parsedCharacters != value.size() || !std::isfinite(parsed) ||
					parsed <= 0.0f || parsed >= 1.0f)
				{
					return false;
				}

				scale = parsed;
				return true;
			}
			catch (const std::exception&)
			{
				return false;
			}
		}
	}

	bool ParseKeyboardKey(const std::string& value, unsigned short& virtualKey)
	{
		const std::string normalized = NormalizeKeyName(value);
		if (normalized.empty())
		{
			return false;
		}

		// Resolve human-friendly names first so users do not need to memorize
		// Windows virtual-key constants for common keys or function keys.
		for (const auto& keyName : FriendlyKeyNames)
		{
			if (normalized == keyName.name)
			{
				virtualKey = keyName.virtualKey;
				return true;
			}
		}

		if (normalized.size() >= 2 && normalized[0] == 'F')
		{
			char* end = nullptr;
			const unsigned long functionNumber = std::strtoul(normalized.c_str() + 1, &end, 10);
			if (end != normalized.c_str() + 1 && *end == '\0' &&
				functionNumber >= 1 && functionNumber <= 24)
			{
				virtualKey = static_cast<unsigned short>(0x70 + functionNumber - 1);
				return true;
			}
		}

		// A single printable key name is interpreted as the key itself before
		// numeric parsing, so "9" binds the 9 key rather than VK_TAB.
		if (normalized.size() == 1)
		{
			const unsigned char character = static_cast<unsigned char>(normalized[0]);
			if ((character >= 'A' && character <= 'Z') ||
				(character >= '0' && character <= '9'))
			{
				virtualKey = character;
				return true;
			}

			switch (character)
			{
			case ';': virtualKey = 0xBA; return true;
			case '=': virtualKey = 0xBB; return true;
			case ',': virtualKey = 0xBC; return true;
			case '-': virtualKey = 0xBD; return true;
			case '.': virtualKey = 0xBE; return true;
			case '/': virtualKey = 0xBF; return true;
			case '`': virtualKey = 0xC0; return true;
			case '[': virtualKey = 0xDB; return true;
			case '\\': virtualKey = 0xDC; return true;
			case ']': virtualKey = 0xDD; return true;
			case '\'': virtualKey = 0xDE; return true;
			// Shifted printable punctuation maps to its underlying keyboard key;
			// the Win32 key-down message reports that virtual key, not the glyph.
			case '!': virtualKey = '1'; return true;
			case '@': virtualKey = '2'; return true;
			case '#': virtualKey = '3'; return true;
			case '$': virtualKey = '4'; return true;
			case '%': virtualKey = '5'; return true;
			case '^': virtualKey = '6'; return true;
			case '&': virtualKey = '7'; return true;
			case '*': virtualKey = '8'; return true;
			case '(': virtualKey = '9'; return true;
			case ')': virtualKey = '0'; return true;
			case '_': virtualKey = 0xBD; return true;
			case '+': virtualKey = 0xBB; return true;
			case '{': virtualKey = 0xDB; return true;
			case '|': virtualKey = 0xDC; return true;
			case '}': virtualKey = 0xDD; return true;
			case ':': virtualKey = 0xBA; return true;
			case '"': virtualKey = 0xDE; return true;
			case '<': virtualKey = 0xBC; return true;
			case '>': virtualKey = 0xBE; return true;
			case '?': virtualKey = 0xBF; return true;
			case '~': virtualKey = 0xC0; return true;
			default: break;
			}
		}

		return ParseNumericKey(normalized, virtualKey);
	}

	Settings LoadSettings(const mINI::INIStructure& ini)
	{
		Settings settings;

		if (ini.has("mouse"))
		{
			const auto mouse = ini.get("mouse");
			unsigned short parsedKey = 0;
			if (mouse.has("toggle") && ParseKeyboardKey(mouse.get("toggle"), parsedKey))
			{
				settings.mouseToggleKey = parsedKey;
			}

			float parsedSensitivity = DefaultSensitivity;
			if (mouse.has("sensitivity") &&
				ParseSensitivity(mouse.get("sensitivity"), parsedSensitivity))
			{
				settings.sensitivity = parsedSensitivity;
			}
		}

		if (!ini.has("window"))
		{
			return settings;
		}

		const auto window = ini.get("window");
		unsigned short parsedKey = 0;
		if (window.has("toggle") && ParseKeyboardKey(window.get("toggle"), parsedKey))
		{
			settings.windowToggleKey = parsedKey;
		}

		// Fixed dimensions are meaningful only when fixed mode is explicitly
		// selected. Invalid fixed values switch to the documented 80% scale mode.
		std::string mode = NormalizeKeyName(window.get("mode"));
		if (mode == "FIXED")
		{
			int width = 0;
			int height = 0;
			if (window.has("window_width") && window.has("window_height") &&
				ParsePositiveInteger(window.get("window_width"), width) &&
				ParsePositiveInteger(window.get("window_height"), height))
			{
				settings.windowSizingMode = WindowSizingMode::Fixed;
				settings.windowWidth = width;
				settings.windowHeight = height;
			}
			else
			{
				// Invalid fixed dimensions explicitly fall back to the documented
				// default scale, not to a possibly customized window_scale value.
				settings.windowSizingMode = WindowSizingMode::Scale;
				settings.windowScale = DefaultWindowScale;
			}
		}
		// Unknown modes also use the scale default rather than guessing that the
		// user intended fixed dimensions.
		else if (mode.empty() || mode == "SCALE")
		{
			float scale = DefaultWindowScale;
			if (window.has("window_scale") &&
				ParseWindowScale(window.get("window_scale"), scale))
			{
				settings.windowScale = scale;
			}
		}

		return settings;
	}
}
