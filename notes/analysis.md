# Terminator: Resistance (UE 4.25, Steam build, exe 72878080 B) - input analysis
All addresses are RVAs (image base 0x140000000, ASLR).

## Input device state
- g_bUsingController: global byte RVA 0x4199e70 (1 = gamepad glyphs, 0 = KB/M).
- IsUsingController 0xba45a0; setter SetUsingController 0xbb6840 (mov [g],dl; jmp notifier 0xb90f70).
- Notifier calls BP HandleUsingController on widgets + broadcasts delegate at 0x4199fc8.
- Writers: 0xba722f 0xba7304 0xba7acb 0xba7c8a 0xba7cf5 0xba7df0 0xba7e77 0xba7ed9 0xbb6840.
- GEngine 0x4413c78.

## What switches it
- PlayerController InputAxis override 0xb274f0: cmp [g],bGamepad; je 0xb27524; call SetUsingController(bGamepad). MouseX/Y -> g=0.
- InputKey override 0xb275d0 same (keys/mouse buttons) - left alone.
- Widget base MouseMove 0xba7c50 (g=0 if delta != 0), menu widget MouseMove 0xba7cc0 (g=0).
- bAlwaysGamepadIcons is a BP param of InterpretText (not config).
- bDisableGamepadDetection (UMyGameUserSettings+0x415) disables XInput entirely - keep False.

## Look processing keyed by g (gamepad curve/scales)
- AddYawInput 0xb109d0 (check 0xb109db), AddPitchInput 0xb108c0 (check 0xb108cb), accel curve 0xb175d0.
- Pawn checks: 0xb38c40, 0xb60625, 0xb631ba, 0xb631e1, 0xb7b5da, 0xb7b601.
- With g=0 the mouse path is linear.

## Mouse
- Raw input: GetRawInputData IAT 0x300c028 (WM_INPUT handler 0x101c319/0x101c361). XInputGetState ord 2 IAT 0x300c358, caller SendControllerEvents 0x101f590.
- Axis mappings: Turn = MouseX + Gamepad_RightX; LookUpMouse = MouseY*-1; mouse sens 0.07; stick deadzone 0.25.
- Shipped DefaultInput.ini: bEnableMouseSmoothing=True (override in Saved/Config/WindowsNoEditor/Input.ini).

## Patch plan
A (glyphs): 0xb27524 `74 09 40 0f b6 d7 e8` -> 73 (je->jae); 0xba7c77 `74 1d 0f 57 c0 41 0f` -> eb; 0xba7cf0 `75 16 48 8b cb 88 05` -> eb.
B (look curve follows a mod flag at RVA 0x4473F00, in .data slack page, set =1 while right stick > ~0.24):
| Site | Expected | New |
| 0xb109db | 803d8e94680300 | 803d1e35960300 |
| 0xb108cb | 803d9e95680300 | 803d2e36960300 |
| 0xb38c40 | 803d2912660300 | 803db9b2930300 |
| 0xb60625 | 803d4498630300 | 803dd438910300 |
| 0xb631ba | 803daf6c630300 | 803d3f0d910300 |
| 0xb631e1 | 803d886c630300 | 803d180d910300 |
| 0xb7b5da | 803d8fe8610300 | 803d1f898f0300 |
| 0xb7b601 | 803d68e8610300 | 803df8888f0300 |
