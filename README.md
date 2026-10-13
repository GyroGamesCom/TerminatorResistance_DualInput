# Dual Input for Terminator: Resistance

Gyro aiming and gamepad at the same time. Gyro moves the camera like a real mouse
(linear, no stick curve), and the game keeps showing gamepad buttons.

## What it does

- Gyro goes straight into the camera as native mouse look (mouse smoothing off).
- Mouse movement no longer switches the prompts to keyboard.
- The right stick/trackpad has priority: while you push it, gyro is ignored.
- **F10** turns the mod on/off in game.

## Install

1. Close the game. Run `install.bat`.
2. Steam Input (controller layout for the game):
   - Gyro: **As Mouse**
   - Right trackpad / right stick: **Joystick**

**Manual install:** copy `xinput1_3.dll` and `xinput_mouse.ini` from `mod` to
`Terminator Resistance\Terminator\Binaries\Win64\`.

## Uninstall

Run `uninstall.bat`, or delete `xinput1_3.dll`, `xinput_mouse.ini` and `mixed_input*.log`
from that folder.

## Settings

`xinput_mouse.ini` next to the DLL. Restart the game after a change.

| Key | Default | |
|---|---|---|
| `GyroSensitivity` | 1.0 | Gyro speed (on top of the in-game mouse sensitivity) |
| `InvertX` / `InvertY` | 0 | 1 = flip that gyro axis |
| `StickOverrideDeadzone` | 0.20 | How far the right stick must move to override gyro |
| `Enabled` | 1 | 0 = mod off |

## Problems

- **No effect:** after starting the game, `mixed_input.log` must appear next to the DLL.
- **Prompts still switch to keyboard:** a Steam Input binding sends real keys or mouse
  clicks. Bind them to gamepad buttons.
- **Game updated, mod stopped working:** the mod skips any game code that changed.
- Tested on the Steam version 1.060.

Source code is in `src` (build with `src\build.bat`, MSVC x64).

Unofficial fan mod, not affiliated with Reef Entertainment, Teyon, Skydance or Valve.
Use at your own risk. License: `LICENSE.txt`.
