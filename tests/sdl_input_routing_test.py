from pathlib import Path


source = Path("src/main.c").read_text(encoding="utf-8")
input_source = Path("src/input.c").read_text(encoding="utf-8")

init_position = source.find("SDL_Init(")
background_hint_position = source.find("SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS")
input_init_position = source.find("input_init()")

if init_position < 0:
    raise SystemExit("SDL_Init call is missing")
if input_init_position < init_position:
    raise SystemExit("input_init must run after SDL_Init")
if background_hint_position < 0 or background_hint_position > init_position:
    raise SystemExit("Background controller events must be enabled before SDL_Init")

# Hardware A/B testing on webOS 6.5.3 showed that SDL's automatic PS5 HIDAPI
# route delivers launcher input. Forcing the same Bluetooth pad to eventN made
# SDL enumerate and open it without delivering controller events.
for forced_route in (
    "SDL_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES",
    "SDL_HINT_JOYSTICK_DEVICE",
    "SDL_HINT_JOYSTICK_HIDAPI_PS5",
    "SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER)",
    "sdl_recover_joystick_path",
):
    if forced_route in source:
        raise SystemExit(f"Forced DualSense routing must stay disabled: {forced_route}")

if 'SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0")' in source:
    raise SystemExit("SDL HIDAPI must remain enabled")

if "dualsense_feedback_new(" in input_source:
    raise SystemExit(
        "Fork-based Luna feedback must remain disabled in the streaming process"
    )

for safe_output_wiring in (
    "rumble_policy_prepare",
    "SDL_JoystickCurrentPowerLevel",
    "SDL is the sole controller-output path",
):
    if safe_output_wiring not in input_source:
        raise SystemExit(
            f"Safe DualSense output/diagnostic wiring is missing: {safe_output_wiring}"
        )

for feature_wiring in (
    "SDL_CONTROLLERTOUCHPADDOWN",
    "SDL_CONTROLLERTOUCHPADMOTION",
    "SDL_CONTROLLERTOUCHPADUP",
    "SDL_CONTROLLERSENSORUPDATE",
    "SDL_GameControllerHasSensor",
    "SDL_GameControllerSetSensorEnabled",
    "controller_features_handle_touch",
    "controller_features_handle_sensor",
    "CHIAKI_EVENT_MOTION_RESET",
):
    if feature_wiring not in input_source:
        raise SystemExit(
            f"DualSense touch/motion wiring is missing: {feature_wiring}"
        )

release = "release-2.30.12-webos.5"
checksum = "4ad566453d113bdd9ee96878176b97d28d9fa70503a62d83d55a351545abb334"
for path in (Path("build-webos.sh"), Path(".github/workflows/build-ipk.yml")):
    contents = path.read_text(encoding="utf-8")
    if release not in contents or checksum not in contents:
        raise SystemExit(f"{path} does not pin the hardware-tested SDL-webOS build")

print("SDL automatic DualSense routing regression test passed")
