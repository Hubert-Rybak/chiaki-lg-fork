from pathlib import Path


source = Path("src/main.c").read_text(encoding="utf-8")
input_source = Path("src/input.c").read_text(encoding="utf-8")
config_header = Path("src/config.h").read_text(encoding="utf-8")
config_source = Path("src/config.c").read_text(encoding="utf-8")

init_position = source.find("SDL_Init(")
background_hint_position = source.find("SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS")
ps5_rumble_hint_position = source.find("SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE")
input_init_position = source.find("input_init(")

if init_position < 0:
    raise SystemExit("SDL_Init call is missing")
if input_init_position < init_position:
    raise SystemExit("input_init must run after SDL_Init")
if background_hint_position < 0 or background_hint_position > init_position:
    raise SystemExit("Background controller events must be enabled before SDL_Init")
if ps5_rumble_hint_position < 0 or ps5_rumble_hint_position > init_position:
    raise SystemExit(
        "DualSense Bluetooth policy must be applied before SDL_Init"
    )
if "SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE" not in source:
    raise SystemExit("DualSense Bluetooth policy must override inherited SDL hints")
if 'cfg.dualsense_bluetooth_enhanced ? "1" : "0"' not in source:
    raise SystemExit("DualSense Bluetooth enhanced mode must be config-gated")
if "bool     dualsense_bluetooth_enhanced;" not in config_header:
    raise SystemExit("DualSense Bluetooth config field is missing")
if 'root, "dualsense_bluetooth_enhanced", false' not in config_source:
    raise SystemExit("DualSense Bluetooth enhanced mode must default off")
if 'SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1")' in source:
    raise SystemExit("DualSense Bluetooth enhanced mode must not be unconditional")
if "input_init(cfg.dualsense_bluetooth_enhanced)" not in source:
    raise SystemExit("DualSense output policy must reach the input subsystem")

# Hardware A/B testing on webOS 6.5.3 showed that SDL's automatic PS5 HIDAPI
# route delivers launcher input. Forcing the same Bluetooth pad to eventN made
# SDL enumerate and open it without delivering controller events.
for forced_route in (
    "SDL_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES",
    "SDL_HINT_JOYSTICK_DEVICE",
    "SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER)",
    "sdl_recover_joystick_path",
):
    if forced_route in source:
        raise SystemExit(f"Forced DualSense routing must stay disabled: {forced_route}")

if "SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5," in source:
    raise SystemExit("The DualSense HIDAPI backend must remain automatically selected")

if 'SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0")' in source:
    raise SystemExit("SDL HIDAPI must remain enabled")

if "dualsense_feedback_new(" in input_source:
    raise SystemExit(
        "Fork-based Luna feedback must remain disabled in the streaming process"
    )

for safe_output_wiring in (
    "rumble_policy_prepare",
    "SDL_GameControllerHasRumble",
    "SDL_JoystickCurrentPowerLevel",
    "First PS5 haptics frame",
    "First non-zero PS5 haptics frame",
    "First non-zero Remote Play rumble event",
    "First SDL rumble write",
    "SDL is the sole controller-output path",
    "controller_transport_from_path",
    "sdl_rumble_available && dualsense_advanced_allowed",
    "fully power it off, then reconnect it for stable basic mode",
):
    if safe_output_wiring not in input_source:
        raise SystemExit(
            f"Safe DualSense output/diagnostic wiring is missing: {safe_output_wiring}"
        )

for capability_guard in (
    "if (ctx->rumble_available)",
    "if (ctx->controller && rumble_available)",
    "rumble_available && rumble_policy_prepare",
):
    if capability_guard not in input_source:
        raise SystemExit(
            f"Controller rumble call is missing its capability guard: {capability_guard}"
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
