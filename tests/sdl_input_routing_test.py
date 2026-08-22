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

device_hint_position = source.find("SDL_HINT_JOYSTICK_DEVICE")
if device_hint_position < 0 or device_hint_position > init_position:
    raise SystemExit("Explicit DualSense evdev path must be configured before SDL_Init")

ps5_hint_position = source.find("SDL_HINT_JOYSTICK_HIDAPI_PS5")
if ps5_hint_position < 0 or ps5_hint_position > init_position:
    raise SystemExit("PS5 HIDAPI driver must yield to evdev before SDL_Init")

hint_call = source[hint_position:init_position]
for device_id in ("0x054c/0x0ce6", "0x054c/0x0df2"):
    if device_id not in hint_call:
        raise SystemExit(f"Bluetooth evdev routing is missing {device_id}")

if 'SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0")' in source:
    raise SystemExit("Do not disable HIDAPI globally; only Bluetooth DualSense is affected")

release = "release-2.30.12-webos.5"
checksum = "4ad566453d113bdd9ee96878176b97d28d9fa70503a62d83d55a351545abb334"
for path in (Path("build-webos.sh"), Path(".github/workflows/build-ipk.yml")):
    contents = path.read_text(encoding="utf-8")
    if release not in contents or checksum not in contents:
        raise SystemExit(f"{path} does not pin the hardware-tested SDL-webOS build")

print("SDL Bluetooth DualSense evdev routing test passed")
