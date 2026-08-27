import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH_PATH = (
    ROOT / "patches" / "sdl-webos-2.30.12-webos.5-ps5-bt-input-only.patch"
)
HINT = "SDL_HINT_JOYSTICK_HIDAPI_PS5_WEBOS_INPUT_ONLY"
REVISION = "9f30a1f01e4d36aa5d3e4e04df5921b7a9d093ee"
SOURCE_SHA256 = "6be84fdf3792009cbf5ac94563957d5e742135d6c6aa8596bf930290332779ca"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def function_body(source: str, name: str) -> str:
    definition = re.search(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{", source)
    require(definition is not None, f"patched SDL source is missing {name}")
    brace = definition.end() - 1

    depth = 0
    for position in range(brace, len(source)):
        character = source[position]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[brace : position + 1]
    raise SystemExit(f"patched SDL source has an unterminated body for {name}")


def check_repository_wiring() -> None:
    patch = PATCH_PATH.read_text(encoding="utf-8")
    changed_files = [
        line.removeprefix("diff --git a/").split(" b/", 1)[0]
        for line in patch.splitlines()
        if line.startswith("diff --git a/")
    ]
    require(
        changed_files
        == ["include/SDL_hints.h", "src/joystick/hidapi/SDL_hidapi_ps5.c"],
        "SDL patch must remain limited to the public hint and PS5 HIDAPI driver",
    )

    for token in (
        HINT,
        "device->is_bluetooth",
        "USB_VENDOR_SONY",
        "USB_PRODUCT_SONY_DS5",
        "USB_PRODUCT_SONY_DS5_EDGE",
        "webos_bluetooth_input_only",
        "HIDAPI_DriverPS5_TickleBluetooth",
        "HIDAPI_DriverPS5_LoadCalibrationData",
        "HIDAPI_DriverPS5_SendJoystickEffect",
    ):
        require(token in patch, f"SDL input-only patch is missing {token}")

    build_script = (ROOT / "build-webos.sh").read_text(encoding="utf-8")
    workflow = (ROOT / ".github/workflows/build-ipk.yml").read_text(
        encoding="utf-8"
    )
    for path, contents in (
        ("build-webos.sh", build_script),
        (".github/workflows/build-ipk.yml", workflow),
    ):
        require(REVISION in contents, f"{path} does not pin the SDL source commit")
        require(
            SOURCE_SHA256 in contents,
            f"{path} does not pin the SDL source archive checksum",
        )

    for token in (
        "https://codeload.github.com/webosbrew/SDL-webOS/tar.gz/$SDL2_WEBOS_REF",
        str(PATCH_PATH.relative_to(ROOT)).replace("\\", "/"),
        'patch --directory="$source_dir" --strip=1 --fuzz=0 --batch',
        '-DWEBOS=ON',
        '-DSDL_DBUS=OFF',
        '-DSDL_ESD=OFF',
        '-DSDL_IBUS=OFF',
        '-DSDL_JACK=OFF',
        '-DSDL_PIPEWIRE=OFF',
        '-DSDL_SNDIO=OFF',
        '-DSDL_STATIC=OFF',
        '-DSDL_TEST=OFF',
        '-DSDL_WEBOS_BROKEN_ABI=OFF',
        '-DSDL_WAYLAND_LIBDECOR=OFF',
        'cmake --build "$build_dir"',
        'cmake --install "$build_dir"',
        'tests/sdl_webos_input_only_patch_test.py" "$source_dir"',
    ):
        require(token in build_script, f"SDL source build is missing {token}")
    require(
        '#!/usr/bin/env bash\nset -o pipefail\n"$REAL_PKG_CONFIG"' in build_script,
        "cross pkg-config wrapper must preserve missing-package failures",
    )
    require(
        '$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig' in build_script,
        "cross pkg-config search path must include target share metadata",
    )
    require(
        "releases/download/$SDL2_WEBOS_RELEASE" not in build_script,
        "SDL-webOS must be compiled from source, not installed from a release binary",
    )
    require(
        "SDL2_WEBOS_SHA256=" not in build_script,
        "obsolete prebuilt SDL checksum is still wired into the build",
    )
    require(
        str(PATCH_PATH.relative_to(ROOT)).replace("\\", "/") in workflow,
        "dependency cache key does not include the SDL patch",
    )

    main_source = (ROOT / "src/main.c").read_text(encoding="utf-8")
    hint_position = main_source.find(HINT)
    init_position = main_source.find("SDL_Init(")
    require(
        0 <= hint_position < init_position,
        "webOS input-only hint must be set before SDL initialization",
    )
    webos_guard = main_source.rfind("#ifdef __WEBOS__", 0, hint_position)
    webos_end = main_source.find("#endif", hint_position)
    require(
        webos_guard >= 0 and hint_position < webos_end < init_position,
        "custom SDL hint must only be set by the webOS build",
    )
    hint_call = main_source[hint_position : main_source.find(");", hint_position) + 2]
    require(
        "dualsense_enhanced_hint" in hint_call,
        "custom SDL hint must use the DualSense enhanced config gate",
    )
    require(
        "cfg.dualsense_bluetooth_enhanced && dualsense_writer_start()"
        in main_source,
        "DualSense enhanced writer config gate is missing",
    )
    require(
        'dualsense_bluetooth_runtime_active ? "1" : "0"' in main_source,
        "DualSense SDL policy must require the verified runtime",
    )


def check_materialized_sdl_source(source_root: Path) -> None:
    hints = (source_root / "include/SDL_hints.h").read_text(encoding="utf-8")
    ps5 = (
        source_root / "src/joystick/hidapi/SDL_hidapi_ps5.c"
    ).read_text(encoding="utf-8")
    require(
        f'#define {HINT} "SDL_JOYSTICK_HIDAPI_PS5_WEBOS_INPUT_ONLY"' in hints,
        "patched SDL headers do not publish the input-only hint",
    )

    selector = function_body(ps5, "HIDAPI_DriverPS5_IsWebOSBluetoothInputOnly")
    for token in (
        "#ifdef __WEBOS__",
        "device->is_bluetooth",
        "device->vendor_id == USB_VENDOR_SONY",
        "device->product_id == USB_PRODUCT_SONY_DS5",
        "device->product_id == USB_PRODUCT_SONY_DS5_EDGE",
        HINT,
    ):
        require(token in selector, f"input-only selector is missing {token}")

    init_device = function_body(ps5, "HIDAPI_DriverPS5_InitDevice")
    require(
        init_device.count(
            "ctx->webos_bluetooth_input_only = "
            "HIDAPI_DriverPS5_IsWebOSBluetoothInputOnly(device);"
        )
        == 2,
        "input-only policy must be latched for simple and enhanced Bluetooth reports",
    )
    feature_guard = "if (ctx->enhanced_mode && !ctx->webos_bluetooth_input_only)"
    require(
        feature_guard in init_device
        and init_device.find(feature_guard)
        < init_device.find("k_EPS5FeatureReportIdSerialNumber")
        < init_device.find("k_EPS5FeatureReportIdFirmwareInfo"),
        "Bluetooth serial and firmware feature reads are not input-only guarded",
    )

    supported = function_body(ps5, "HIDAPI_DriverPS5_IsSupportedDevice")
    require(
        supported.find("if (type == SDL_CONTROLLER_TYPE_PS5)")
        < supported.find("ReadFeatureReport"),
        "known DualSense devices must be accepted before third-party feature probing",
    )
    controller_list_path = source_root / "src/joystick/controller_list.h"
    if controller_list_path.exists():
        controller_list = controller_list_path.read_text(encoding="utf-8")
        for controller_id in ("0x054c, 0x0ce6", "0x054c, 0x0df2"):
            entry = next(
                (
                    line
                    for line in controller_list.splitlines()
                    if controller_id in line
                ),
                "",
            )
            require(
                "k_eControllerType_PS5Controller" in entry,
                f"upstream SDL no longer classifies {controller_id} as a PS5 controller",
            )

    calibration = function_body(ps5, "HIDAPI_DriverPS5_LoadCalibrationData")
    require(
        calibration.find("if (ctx->webos_bluetooth_input_only)")
        < calibration.find("k_EPS5FeatureReportIdCalibration"),
        "calibration feature read is not input-only guarded",
    )

    update_effects = function_body(ps5, "HIDAPI_DriverPS5_UpdateEffects")
    send_effect = function_body(ps5, "HIDAPI_DriverPS5_SendJoystickEffect")
    tickle = function_body(ps5, "HIDAPI_DriverPS5_TickleBluetooth")
    capabilities = function_body(ps5, "HIDAPI_DriverPS5_GetJoystickCapabilities")
    for name, body in (
        ("effect update", update_effects),
        ("raw effect send", send_effect),
        ("Bluetooth tickle", tickle),
        ("output capabilities", capabilities),
    ):
        require(
            "if (ctx->webos_bluetooth_input_only" in body,
            f"{name} is not blocked by the input-only policy",
        )
    require(
        ps5.count("SDL_HIDAPI_SendRumbleAndUnlock(") == 2,
        "unexpected PS5 HIDAPI output primitive was added",
    )
    require(
        "SDL_hid_write(" not in ps5 and "SDL_hid_send_feature_report(" not in ps5,
        "unexpected direct PS5 HID output path bypasses the policy",
    )

    enhanced_mode = function_body(ps5, "HIDAPI_DriverPS5_SetEnhancedMode")
    for token in (
        "SDL_PrivateJoystickAddTouchpad",
        "SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO",
        "SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL",
        "if (!ctx->webos_bluetooth_input_only)",
    ):
        require(token in enhanced_mode, f"enhanced input topology is missing {token}")
    require(
        enhanced_mode.find("if (!ctx->webos_bluetooth_input_only)")
        < enhanced_mode.find("HIDAPI_DriverPS5_UpdateEffects"),
        "enhanced input promotion can emit an SDL initialization effect",
    )

    open_joystick = function_body(ps5, "HIDAPI_DriverPS5_OpenJoystick")
    require(
        "else if (!ctx->webos_bluetooth_input_only)" in open_joystick
        and "if (!ctx->webos_bluetooth_input_only)" in open_joystick
        and "return SDL_TRUE" in open_joystick,
        "input-only joystick open must avoid output callbacks without rejecting input",
    )

    update_device = function_body(ps5, "HIDAPI_DriverPS5_UpdateDevice")
    for token in (
        "k_EPS5ReportIdBluetoothState",
        "HIDAPI_DriverPS5_HandleStatePacketCommon",
        "HIDAPI_DriverPS5_HandleStatePacket",
        "!ctx->webos_bluetooth_input_only",
    ):
        require(token in update_device, f"enhanced Bluetooth parsing is missing {token}")
    bluetooth_case_start = update_device.find("case k_EPS5ReportIdBluetoothState:")
    bluetooth_case_end = update_device.find("default:", bluetooth_case_start)
    bluetooth_case = update_device[bluetooth_case_start:bluetooth_case_end]
    require(
        0
        <= bluetooth_case.find("if (!ctx->enhanced_mode)")
        < bluetooth_case.find("HIDAPI_DriverPS5_SetEnhancedMode")
        < bluetooth_case.find("HIDAPI_DriverPS5_HandleStatePacketCommon")
        < bluetooth_case.find("HIDAPI_DriverPS5_HandleStatePacket("),
        "an externally promoted simple pad must gain topology before parsing its report",
    )

    sensors = function_body(ps5, "HIDAPI_DriverPS5_SetJoystickSensorsEnabled")
    require(
        "enabled && !ctx->webos_bluetooth_input_only" in sensors
        and "ctx->report_sensors = enabled" in sensors,
        "sensor parsing must remain enabled without a calibration feature read",
    )
    for forbidden in (
        "HIDAPI_DriverPS5_UpdateEffects",
        "HIDAPI_DriverPS5_SendJoystickEffect",
        "HIDAPI_DriverPS5_TickleBluetooth",
        "ReadFeatureReport",
    ):
        require(forbidden not in sensors, f"sensor enable unexpectedly invokes {forbidden}")


check_repository_wiring()
if len(sys.argv) > 2:
    raise SystemExit("usage: sdl_webos_input_only_patch_test.py [patched-sdl-source]")
if len(sys.argv) == 2:
    check_materialized_sdl_source(Path(sys.argv[1]).resolve())

print("SDL-webOS DualSense Bluetooth input-only patch regression test passed")
