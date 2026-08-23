import re
from pathlib import Path


install = Path("packaging/root/install.sh").read_text(encoding="utf-8")
load = Path("packaging/root/load.sh").read_text(encoding="utf-8")
boot = Path("packaging/root/boot-hook.sh").read_text(encoding="utf-8")
uninstall = Path("packaging/root/uninstall.sh").read_text(encoding="utf-8")
workflow = Path(".github/workflows/build-ipk.yml").read_text(encoding="utf-8")
all_root_scripts = "\n".join((install, load, boot, uninstall))

for line in all_root_scripts.splitlines():
    command = line.strip()
    if not command or command.startswith("#"):
        continue
    if re.search(r"\b(?:insmod|modprobe|rmmod)\b", command):
        raise SystemExit(f"Root script can activate or remove a module: {command}")
    if re.search(r"/sys/bus/hid/drivers/[^ ]+/(?:bind|unbind)\b", command):
        raise SystemExit(f"Root script can rebind a HID device: {command}")

for unsafe_selector in (
    "aarch64:4.4.84*",
    "armv7l:4.4.84-*.koli.*",
    'insmod "$MODULE"',
    "rmmod hid_playstation",
    "/sys/bus/hid/drivers/playstation/bind",
    "/sys/bus/hid/drivers/playstation/unbind",
    "/sys/bus/hid/drivers/hid-generic/bind",
    '"$PATCHER" "$bluetooth_pid"',
):
    if unsafe_selector in all_root_scripts:
        raise SystemExit(
            f"Unverified compatibility activation escaped quarantine: {unsafe_selector}"
        )

for required_cleanup in (
    "Removing legacy unverified DualSense compatibility installation",
    '/bin/sh "$BUNDLE_ROOT/root/uninstall.sh"',
    "No ABI-verified compatibility module is bundled",
    "chiaki-dualsense-reboot-required",
):
    if required_cleanup not in install:
        raise SystemExit(f"Installer safe cleanup is missing: {required_cleanup}")
if 'elif [ "$module_loaded" = false ]; then' not in install:
    raise SystemExit("Reboot warning would not persist until the module disappears")
for marker_safety in (
    'mktemp "${REBOOT_MARKER}.XXXXXX"',
    'mv -f "$marker_tmp" "$REBOOT_MARKER"',
):
    if marker_safety not in install:
        raise SystemExit(f"Root reboot marker is not created atomically: {marker_safety}")

if "refusing patch, module load, and rebind" not in load:
    raise SystemExit("Legacy boot loader does not fail closed")
if "exit 0" not in boot or "load.sh" in boot:
    raise SystemExit("Packaged compatibility boot hook must remain inert")
for forbidden_cleanup in (
    "rmmod hid_playstation",
    "/sys/bus/hid/drivers/playstation/unbind",
    "/sys/bus/hid/drivers/hid-generic/bind",
    'rm -rf "$STATE_DIR"',
):
    if forbidden_cleanup in uninstall or forbidden_cleanup in load:
        raise SystemExit(
            f"Cleanup could disturb an LG-provided HID driver: {forbidden_cleanup}"
        )
if "Never unbind or unload by module name" not in uninstall:
    raise SystemExit("Uninstaller is missing the native-driver safety policy")
for rollback_wiring in (
    'rm -f "$HOOK"',
    '[ -e "$PATCH_MARKER" ]',
    'retaining helper for retry',
    'exit 75',
):
    if rollback_wiring not in uninstall:
        raise SystemExit(f"Live Bluetooth rollback is not retry-safe: {rollback_wiring}")
if '[ -s "$PATCH_MARKER" ]' in uninstall:
    raise SystemExit("An interrupted zero-byte rollback marker would be discarded")

for staging_guard in (
    'PATTERN "modules" EXCLUDE',
    'PATTERN "chiaki-bt-patch" EXCLUDE',
    'remove_directory "${IPK_STAGING_DIR}"',
):
    if staging_guard not in Path("CMakeLists.txt").read_text(encoding="utf-8"):
        raise SystemExit(f"Incremental IPK staging is not fail-closed: {staging_guard}")

for unsafe_packaging in (
    "Build webOS 4.4 DualSense kernel modules",
    "root/modules/aarch64",
    "root/modules/armv7l",
):
    if unsafe_packaging in workflow:
        raise SystemExit(f"Unverified kernel module is still packaged: {unsafe_packaging}")

print("Root compatibility policy fails closed")
