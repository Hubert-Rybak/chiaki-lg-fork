from pathlib import Path


source = Path("src/main.c").read_text(encoding="utf-8")

hint = '"SDL_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES"'
hint_position = source.find(hint)
init_position = source.find("SDL_Init(")
input_init_position = source.find("input_init()")

if hint_position < 0:
    raise SystemExit("Bluetooth DualSense HIDAPI exclusion hint is missing")
if init_position < 0:
    raise SystemExit("SDL_Init call is missing")
if input_init_position < 0:
    raise SystemExit("input_init call is missing")
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

conditional_hint = "const SDL_bool dualsense_evdev_hint = !dualsense_event_found ||"
if conditional_hint not in source:
    raise SystemExit("Bluetooth HIDAPI must remain available without an evdev node")

recovery_call = "sdl_recover_joystick_path(dualsense_event_path)"
recovery_position = source.rfind(recovery_call)
if recovery_position < init_position or recovery_position > input_init_position:
    raise SystemExit("The startup race recovery must run after SDL_Init and before input_init")

for recovery_contract in (
    "SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER)",
    "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)",
    "#define DUALSENSE_SDL_RETRY_COUNT 20",
    "#define DUALSENSE_SDL_RETRY_MS    100",
):
    if recovery_contract not in source:
        raise SystemExit(f"SDL startup recovery is missing {recovery_contract}")

release = "release-2.30.12-webos.5"
checksum = "4ad566453d113bdd9ee96878176b97d28d9fa70503a62d83d55a351545abb334"
for path in (Path("build-webos.sh"), Path(".github/workflows/build-ipk.yml")):
    contents = path.read_text(encoding="utf-8")
    if release not in contents or checksum not in contents:
        raise SystemExit(f"{path} does not pin the hardware-tested SDL-webOS build")

print("SDL Bluetooth DualSense evdev routing test passed")
