# Dual Input mod for Terminator: Resistance

Use **gyro aiming and a gamepad at the same time**, with native mouse-look feel and
gamepad button prompts.

Out of the box, Terminator: Resistance switches its button prompts to keyboard as soon
as the mouse moves. Steam Input sends gyro as mouse, so on Steam Deck, Steam Controller,
DualSense, Switch Pro and similar controllers you keep losing the gamepad glyphs. And if
the game is told to stay in gamepad mode, it runs the mouse through the right-stick
acceleration curve. This mod fixes both:

- **Gamepad glyphs stay.** Mouse (gyro) movement no longer switches the game to
  keyboard prompts. Real keyboard keys and mouse clicks still do, and any gamepad input
  switches back.
- **Gyro is native, linear mouse look.** No stick curve, no acceleration. The game's
  gamepad look curve is only used while the right stick/trackpad is actually pushed.
  UE4 mouse smoothing is turned off too.
- **The right stick/trackpad wins.** While you push it, gyro is ignored (plus a short
  hold after you let go), so big turns stay clean and gyro handles fine aim.
- **F10 toggles** the mod on and off at runtime.
- Everything else (left stick, buttons, triggers, vibration) passes through untouched.

Unofficial fan mod. Not affiliated with Reef Entertainment, Teyon, Skydance or Valve.
Use at your own risk.

## Requirements

- Terminator: Resistance, **Steam version** (made for game version 1.060,
  `Terminator-Win64-Shipping.exe` dated 2021-12-14). Other builds: the mod checks the
  game code before patching and skips whatever does not match.
- A controller with gyro, used through **Steam Input**.

## Install

Download the latest ZIP from the [Releases](https://github.com/GyroGamesCom/TR_DualInput/releases) page.

**Automatic:** unzip anywhere and run `install.bat`. It finds the game through
Steam (or asks for the folder), backs up any `xinput1_3.dll` it would replace, keeps
your existing settings if you update, and turns off mouse smoothing in the game's
user `Input.ini`.

**Manual:** copy the 2 files from the `mod` folder (`xinput1_3.dll`, `xinput_mouse.ini`)
next to `Terminator-Win64-Shipping.exe`, which is in
`Terminator Resistance\Terminator\Binaries\Win64\`
(Steam: right-click the game, then Manage > Browse local files).
Optionally add these lines to
`%LOCALAPPDATA%\Terminator\Saved\Config\WindowsNoEditor\Input.ini`:

```
[/Script/Engine.InputSettings]
bEnableMouseSmoothing=False
```

**Uninstall:** run `uninstall.bat`, or delete `xinput1_3.dll`, `xinput_mouse.ini`,
`mixed_input.log` and `mixed_input.old.log` from that folder.

## Steam Input setup

In the game's controller configuration:

| Input              | Set to                                 |
|--------------------|----------------------------------------|
| Gyro               | **As Mouse** (any activation you like) |
| Right trackpad / stick | **Joystick** (right stick output)  |
| Everything else    | Normal gamepad layout                  |

Tune each input separately:

- **Gyro speed:** the in-game mouse sensitivity, `GyroSensitivity` in
  `xinput_mouse.ini`, or the Steam Input gyro settings.
- **Trackpad speed:** Steam Input trackpad settings, or the in-game controller
  sensitivity.

If vertical gyro is inverted, use the in-game mouse invert, `InvertY` in the ini, or
invert in Steam (only one of them, or they cancel out).

## Settings (`xinput_mouse.ini`)

Changes take effect after restarting the game. Every option is explained in the file.

| Key            | Default | Meaning |
|----------------|---------|---------|
| `Enabled`      | 1    | 1 = mod on, 0 = pass-through. F10 toggles at runtime |
| `PadIndex`     | -1   | XInput user index whose right stick has priority (-1 = any pad) |
| `StickOverrideDeadzone` | 0.20 | Right-stick deflection above which gyro is ignored |
| `StickReleaseMs` | 80 | Keep ignoring gyro this long after the stick is released |
| `GyroSensitivity` | 1.0 | Multiplier on gyro (mouse) movement, on top of the in-game mouse sensitivity |
| `InvertX`, `InvertY` | 0 | 1 = invert that gyro axis |
| `ToggleKey`    | 0x79 | Virtual-key code of the on/off key (0x79 = F10, 0 = none) |
| `Patches`      | 1    | 1 = apply the game patches (glyphs + linear gyro). 0 = only stick priority |
| `ForceGamepadAtStart` | 1 | 1 = start in gamepad-prompt mode |
| `PadCurveThreshold` | 0.24 | Right-stick deflection above which the game's gamepad look curve is used |
| `Log`          | 1    | 0 = off, 1 = startup info, 2 = also per-second statistics (`mixed_input.log`) |

## Troubleshooting

- **Nothing changes:** check that `mixed_input.log` appears in
  `Terminator\Binaries\Win64` after launching. If not, the DLL is in the wrong folder.
- **Log says a patch was refused / skipped:** the game build differs from the one the
  mod was made for. Stick priority still works; glyphs and linear gyro need an updated mod.
- **Prompts still switch to keyboard:** something sends real keyboard keys or mouse
  clicks (for example a Steam Input binding). Bind those to gamepad buttons instead.
- **Antivirus warning:** proxy DLLs next to a game exe are a common false positive
  (ReShade and many fixes work the same way). The full source code is in `src`.
- **Conflict with another mod** that also uses `xinput1_3.dll`: the installer backs it
  up to `DualInput_backup` and restores it on uninstall. The two cannot run at the
  same time.

## Limitations

- Gyro is not meant for menus; use the gamepad there.
- Several addresses are specific to the tested game build. After a game update the
  mod checks the code before patching and skips anything that does not match.

## How it works

One small proxy DLL, `xinput1_3.dll`, sits between the game and Windows. The game
imports XInput 1.3 by ordinal, so the DLL placed next to the exe is loaded instead of the
system one, and it forwards everything to the real XInput. Then:

- **`XInputGetState`** is hooked through the exe's import table (Steam's overlay detours
  the export, so the mod calls Steam's version first) to read the right stick.
- **`GetRawInputData`** is hooked to drop gyro (mouse) movement while the right stick is
  pushed. Buttons and wheel are kept.
- The game keeps one "using controller" flag that drives **both** the button prompts and
  the gamepad look processing. The mod patches the game in memory (never on disk) so
  that:
  - mouse movement can no longer clear that flag (3 one-byte patches in the player
    controller's axis handler and the widgets' mouse-move handlers), and
  - the gamepad look curve / sensitivity checks read a separate mod-owned flag that is
    only set while the right stick is pushed, so gyro stays linear.

Every patch site is verified against the expected bytes first; anything that does not
match is left alone and logged.

Build: run `src\build.bat` (needs Visual Studio Build Tools, 64-bit MSVC).

## Contact

Bug reports, questions and permission requests: open an issue at
https://github.com/GyroGamesCom/TR_DualInput/issues

## License

Free for personal use. Redistribution, re-uploading or any commercial use requires prior written
permission from GyroGamesCom, and GyroGamesCom must always be credited. Showing the mod in
videos and streams is fine with credit. See `LICENSE.txt` for the full terms.
