# Lego City Undercover Mouse Support


## Features

- Control the camera using your mouse
- Left & right click can be bound to keyboard buttons
- Adjustable sensiivity

## Installation

- Download latest release
- Extract contents into LEGO City Undercover installation folder (where LEGOLCUR_DX11.exe is located)
- Play!

## Usage

- Scroll adjust camera distance
- Home/End buttons increase/decrease sensitivity by `0.001` in game or adjust it in `lcu.ini`
- F8 enables/disables mouse-camera control. Rebind it with `[mouse] toggle` in `lcu.ini`; friendly names such as `HOME`, single characters, and numeric/hexadecimal virtual-key codes are accepted. Mouse buttons are not valid toggle keys.
- F9 toggles between fullscreen and a normal decorated window. Rebind it with `[window] toggle`. The default `mode=scale` uses `window_scale=0.80` of the largest correctly proportioned client area that fits the current monitor work area. Set `mode=fixed` to choose `window_width` and `window_height` client dimensions instead (default `1280x720`).
- Mouse-camera sensitivity is stored as `[mouse] sensitivity` and is adjusted in game with Home/End. Missing or invalid values use `0.003`.
- By default Left and right click are bound to Q + E. Edit the ini file to change these or adjust your in game mappings accordingly. Use virtual keycodes in ini https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes

## Recent Changes

- Added configurable, keyboard-only F8/F9 toggle bindings, with validation and defaults for invalid settings.
- Added scale-based and fixed-client-size windowed modes while preserving the game's aspect ratio in scale mode.
- `lcu.ini` is created with explanatory comments; updates retain comments while saving Home/End sensitivity changes.
