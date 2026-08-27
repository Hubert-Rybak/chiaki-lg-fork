import re
from pathlib import Path


def shell_function(source: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^{re.escape(name)}\(\)\n\{{\n(.*?)^\}}\n",
        source,
    )
    if not match:
        raise SystemExit(f"Root runtime is missing shell function: {name}")
    return match.group(1)


install = Path("packaging/root/install.sh").read_text(encoding="utf-8")
load = Path("packaging/root/load.sh").read_text(encoding="utf-8")
boot = Path("packaging/root/boot-hook.sh").read_text(encoding="utf-8")
uninstall = Path("packaging/root/uninstall.sh").read_text(encoding="utf-8")
runtime = Path("packaging/root/bluetooth-runtime.sh").read_text(encoding="utf-8")
workflow = Path(".github/workflows/build-ipk.yml").read_text(encoding="utf-8")
root_feedback = Path("src/root_feedback.c").read_text(encoding="utf-8")
main_source = Path("src/main.c").read_text(encoding="utf-8")
all_root_scripts = "\n".join((install, load, boot, uninstall, runtime))

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
    "run_legacy_uninstall",
    "open_verified_source",
    "/proc/self/fd/7",
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

for runtime_guard in (
    'RUNTIME_REQUESTED=false',
    '--dualsense-bluetooth-runtime',
    'RUNTIME_SOURCE_SHA=',
    'UNINSTALL_SOURCE_SHA=',
    '/proc/self/fd/8',
    'chmod 0500 "$runtime_tmp"',
    '"$RUNTIME_STATE_DIR/activation.lock"',
    "APP_EXE_ID=$(stat -Lc '%d:%i' \"/proc/$APP_PID/exe\"",
    "BUNDLE_EXE_ID=$(stat -Lc '%d:%i' \"$BUNDLE_ROOT/chiaki-webos\"",
    '[ "$APP_EXE_ID" = "$BUNDLE_EXE_ID" ]',
    "Unsafe Chiaki executable identity.",
    "Unsafe bundled Chiaki executable.",
):
    if runtime_guard not in install:
        raise SystemExit(f"Root runtime staging is not authenticated: {runtime_guard}")
if "__RUNTIME_SOURCE_SHA256__" in install or "__UNINSTALL_SOURCE_SHA256__" in install:
    raise SystemExit("Root runtime source hashes were not finalized")
if 'APP_EXE=$(readlink -f "/proc/$APP_PID/exe"' in install:
    raise SystemExit("Jailed webOS app identity cannot be compared by pathname")
if '[ "$APP_EXE" = "$BUNDLE_EXE" ]' in install:
    raise SystemExit("Jailed webOS app identity cannot require pathname equality")

for bootstrap_guard in (
    "ROOT_INSTALLER_SHA256",
    "/proc/self/fd/9",
    "/bin/stat -Lc %%u:%%a",
    "/usr/bin/sha256sum /proc/self/fd/9",
):
    if bootstrap_guard not in root_feedback:
        raise SystemExit(f"Root installer bootstrap is not authenticated: {bootstrap_guard}")
if "__ROOT_INSTALLER_SHA256__" in root_feedback:
    raise SystemExit("Root installer hash was not finalized")

# The Homebrew service does not cancel its child_process.exec command when the
# Luna client disappears.  Therefore a timeout cannot safely fall back while
# this same app PID remains alive: the detached root command could still reach
# ACTIVE.  Pin the current fail-closed launch behavior.
bootstrap_call = main_source.find("if (root_feedback_bootstrap(true))")
failure_start = main_source.find("\n        } else {", bootstrap_call)
outer_else = main_source.find("\n    } else {", failure_start + 1)
if min(bootstrap_call, failure_start, outer_else) < 0:
    raise SystemExit("Enhanced root bootstrap failure branch is missing")
failure_branch = main_source[failure_start:outer_else]
failure_order = (
    "dualsense_writer_stop();",
    "config_free(&cfg);",
    "return 1;",
)
failure_positions = [failure_branch.find(token) for token in failure_order]
if any(position < 0 for position in failure_positions) or (
    failure_positions != sorted(failure_positions)
):
    raise SystemExit(
        "Enhanced root bootstrap failure must stop output and exit the app"
    )
root_timeout = re.search(
    r"(?m)^#define ROOT_BOOTSTRAP_TIMEOUT_MS ([0-9]+)$", root_feedback
)
if (
    not root_timeout
    or int(root_timeout.group(1)) < 20000
    or "the enhanced launch must exit fail-closed" not in root_feedback
):
    raise SystemExit("Root bootstrap timeout is not bound to fail-closed launch policy")

for runtime_safety in (
    '"volatile":true',
    "OWNER_KEY=org.webosbrew.chiaki.dualsenseSessionOwner",
    "ALLOWLIST_INTENT",
    "PATCH_INTENT",
    "SIGNATURE_OFF=bb680322da80bb691b79ba68f9691846",
    "SIGNATURE_ON=bb680222da80bb691b79ba68f9691846",
    "BT_LIBRARY_SHA=e0beb5a886d37e5358bf3324a60a207d45a703e87015cdceddfe265ca5e6965f",
    "CHIAKI_RUNTIME_WATCH_FD=9",
    "ACTIVATION_LOCK_FILE",
    "watcher_cleanup",
    "app_matches \"$APP_PID\" \"$APP_START\" \"$APP_EXE_ID\"",
):
    if runtime_safety not in runtime:
        raise SystemExit(f"Experimental Bluetooth runtime is missing: {runtime_safety}")
for forbidden_runtime in (
    '"volatile":false',
    "configd_db.json",
    "configd_factory_db.json",
    "layers/base/com.webos.service.bthidmanager.json >",
    "/tmp/chiaki-dualsense-runtime.log",
):
    if forbidden_runtime in runtime:
        raise SystemExit(f"Bluetooth runtime can persist or corrupt global state: {forbidden_runtime}")

# LG's configd accepts a custom volatile owner value, but its null-delete path
# reports success without deleting that key.  The tested neutral value is an
# empty string (or a genuinely missing key after reboot).  Pin that exact model
# so cleanup cannot silently regress to the stranded-owner behavior seen on TV.
reset_owner = shell_function(runtime, "reset_owner")
expected_owner_reset = (
    'payload=\'{"configs":{"\'"$OWNER_KEY"\'":""},"volatile":true}\''
)
if expected_owner_reset not in reset_owner:
    raise SystemExit("Runtime owner reset must write the verified empty sentinel")
if any("$OWNER_KEY" in line and ":null" in line for line in runtime.splitlines()):
    raise SystemExit("Runtime owner reset cannot use configd's broken null-delete path")

baseline = shell_function(runtime, "baseline_config_is")
baseline_lines = {line.strip() for line in baseline.splitlines()}
for accepted_baseline in (
    '*\'"\'"$OWNER_KEY"\'":""\'*) return 0 ;;',
    '*\'"missingConfigs":\'*\'"\'"$OWNER_KEY"\'"\'*) return 0 ;;',
):
    if accepted_baseline not in baseline_lines:
        raise SystemExit(
            "Runtime baseline must accept only missing or empty owner state"
        )
if '*\'"\'"$OWNER_KEY"\'":\'*) return 1 ;;' not in baseline_lines:
    raise SystemExit("Runtime baseline does not reject a non-empty owner")
if baseline.count("return 0") != 2:
    raise SystemExit("Runtime baseline accepts an unverified owner state")

rollback = shell_function(runtime, "clear_owned_runtime_config")
rollback_order = (
    "clear_allowlist || return 1",
    'allowlist_value_is "$BASE_QUERY_ARRAY" || return 1',
    'owner_is "$OWNER_NONCE" || return 1',
    "reset_owner || return 1",
    "baseline_config_is",
)
positions = [rollback.find(token) for token in rollback_order]
if any(position < 0 for position in positions) or positions != sorted(positions):
    raise SystemExit(
        "Owned rollback must restore and verify the list before resetting ownership"
    )

# Configd emits fields in a different order across calls.  Keep returnValue
# validation separate from the owner/list match rather than one ordered glob.
for helper_name, matched_key in (
    ("owner_is", "$OWNER_KEY"),
    ("allowlist_value_is", "$KEY"),
):
    helper = shell_function(runtime, helper_name)
    return_guard = (
        'case "$response" in *\'"returnValue":true\'*) ;; '
        '*) return 1 ;; esac'
    )
    second_case = helper.find('case "$response" in', helper.find(return_guard) + 1)
    if (
        helper.count('case "$response" in') != 2
        or return_guard not in helper
        or second_case < 0
        or helper.find(matched_key, second_case) < 0
    ):
        raise SystemExit(
            f"{helper_name} must validate JSON fields without assuming key order"
        )

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
