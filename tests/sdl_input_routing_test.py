from pathlib import Path


source = Path("src/main.c").read_text(encoding="utf-8")
input_source = Path("src/input.c").read_text(encoding="utf-8")
config_header = Path("src/config.h").read_text(encoding="utf-8")
config_source = Path("src/config.c").read_text(encoding="utf-8")
dualsense_source = Path("src/dualsense.c").read_text(encoding="utf-8")
root_source = Path("src/root_feedback.c").read_text(encoding="utf-8")

init_position = source.find("SDL_Init(")
background_hint_position = source.find("SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS")
ps5_rumble_hint_position = source.find("SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE")
ps5_webos_input_only_position = source.find(
    "SDL_HINT_JOYSTICK_HIDAPI_PS5_WEBOS_INPUT_ONLY"
)
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
if ps5_webos_input_only_position < 0 or ps5_webos_input_only_position > init_position:
    raise SystemExit(
        "webOS DualSense Bluetooth input-only policy must be applied before SDL_Init"
    )
if "SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE" not in source:
    raise SystemExit("DualSense Bluetooth policy must override inherited SDL hints")
if "cfg.dualsense_bluetooth_enhanced && dualsense_writer_start()" not in source:
    raise SystemExit("DualSense writer startup must be config-gated")
if 'root_feedback_bootstrap(true)' not in source:
    raise SystemExit("Enhanced mode must require the exact-TV root runtime")
if 'dualsense_bluetooth_runtime_active ? "1" : "0"' not in source:
    raise SystemExit("SDL enhanced input must be gated by the ready runtime")
if "bool     dualsense_bluetooth_enhanced;" not in config_header:
    raise SystemExit("DualSense Bluetooth config field is missing")
if 'root, "dualsense_bluetooth_enhanced", false' not in config_source:
    raise SystemExit("DualSense Bluetooth enhanced mode must default off")
if 'SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1")' in source:
    raise SystemExit("DualSense Bluetooth enhanced mode must not be unconditional")
if "input_init(dualsense_bluetooth_runtime_active)" not in source:
    raise SystemExit("DualSense output policy must reach the input subsystem")
if source.find("dualsense_writer_start()") > init_position:
    raise SystemExit("The isolated writer must start before SDL/video initialization")

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

for writer_wiring in (
    "dualsense_feedback_new()",
    "dualsense_feedback_set_rumble",
    "dualsense_feedback_delivery_result",
    "dualsense_feedback_healthy",
    "isolated writer is the sole output path",
):
    if writer_wiring not in input_source:
        raise SystemExit(f"Isolated DualSense writer wiring is missing: {writer_wiring}")
for worker_safety in (
    "socketpair(AF_UNIX, SOCK_SEQPACKET",
    "dualsense_writer_start(void)",
    "PR_SET_PDEATHSIG",
    "WRITER_IDLE_STOP_MS",
    "dualsense_output_state_release",
    "WRITER_REPLY_DELIVERY_FAILED",
    "reply == WRITER_REPLY_TRANSPORT_FAILED",
    "terminate_and_reap_child",
    "CHILD_KILL_WAIT_MS",
):
    if worker_safety not in dualsense_source:
        raise SystemExit(f"Isolated writer safety wiring is missing: {worker_safety}")
if "if (!accepted)" in dualsense_source:
    raise SystemExit(
        "A service-level report failure must not permanently kill the writer"
    )
if "waitpid(child, NULL, 0)" in dualsense_source or \
        "waitpid(failed_child, NULL, 0)" in dualsense_source:
    raise SystemExit("Output child shutdown/reaping must remain bounded")
if "dualsense_writer_start" not in source[:init_position]:
    raise SystemExit("The output worker must be pre-spawned before SDL_Init")
if "ROOT_INSTALLER_SHA256" not in root_source:
    raise SystemExit("Root bootstrap must authenticate its installer source")

for safe_output_wiring in (
    "rumble_policy_prepare",
    "SDL_GameControllerHasRumble",
    "SDL_JoystickCurrentPowerLevel",
    "First PS5 haptics frame",
    "First non-zero PS5 haptics frame",
    "First non-zero Remote Play rumble event",
    "First SDL rumble write",
    "isolated-writer",
    "controller_transport_from_path",
    "feedback != NULL ||",
    "fully power it off, then reconnect it for stable basic mode",
    "refresh_dualsense_enhanced_sensors",
    "DualSense external enhanced promotion observed",
    "SDL_GameControllerSetSensorEnabled",
    "First isolated DualSense rumble delivery confirmed",
    "retrying through the existing writer only",
):
    if safe_output_wiring not in input_source:
        raise SystemExit(
            f"Safe DualSense output/diagnostic wiring is missing: {safe_output_wiring}"
        )

for async_ack_wiring in (
    "ctx->feedback_rumble_pending = true",
    "ctx->feedback_rumble_generation = feedback_generation",
    "delivery_complete && ctx->feedback_rumble_pending",
    "policy_success = delivery_success",
):
    if async_ack_wiring not in input_source:
        raise SystemExit(
            f"Asynchronous DualSense delivery tracking is missing: {async_ack_wiring}"
        )

for capability_guard in (
    "ctx->rumble_available && !ctx->dualsense_feedback",
    "ctx->controller && rumble_available && !feedback",
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
revision = "9f30a1f01e4d36aa5d3e4e04df5921b7a9d093ee"
checksum = "6be84fdf3792009cbf5ac94563957d5e742135d6c6aa8596bf930290332779ca"
for path in (Path("build-webos.sh"), Path(".github/workflows/build-ipk.yml")):
    contents = path.read_text(encoding="utf-8")
    if release not in contents or revision not in contents or checksum not in contents:
        raise SystemExit(f"{path} does not pin the exact SDL-webOS source")

print("SDL automatic DualSense routing regression test passed")
