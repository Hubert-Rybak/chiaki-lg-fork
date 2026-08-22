from pathlib import Path


source = Path("src/main.c").read_text(encoding="utf-8")

hint = '"SDL_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES"'
hint_position = source.find(hint)
init_position = source.find("SDL_Init(")

if hint_position < 0:
    raise SystemExit("Bluetooth DualSense HIDAPI exclusion hint is missing")
if init_position < 0:
    raise SystemExit("SDL_Init call is missing")
if hint_position > init_position:
    raise SystemExit("Bluetooth DualSense routing hint must be set before SDL_Init")

hint_call = source[hint_position:init_position]
for device_id in ("0x054c/0x0ce6", "0x054c/0x0df2"):
    if device_id not in hint_call:
        raise SystemExit(f"Bluetooth evdev routing is missing {device_id}")

if 'SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0")' in source:
    raise SystemExit("Do not disable HIDAPI globally; only Bluetooth DualSense is affected")

print("SDL Bluetooth DualSense evdev routing test passed")
