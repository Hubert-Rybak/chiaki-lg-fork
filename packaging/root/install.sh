#!/bin/sh
set -eu

BUNDLE_ROOT=
if [ "$#" -gt 0 ]; then
    BUNDLE_ROOT=$1
    shift
fi
RUNTIME_REQUESTED=false
APP_PID=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --rebind-connected)
            shift # accepted for older app binaries
            ;;
        --dualsense-bluetooth-runtime)
            [ "$#" -eq 2 ] || { echo "Missing runtime app PID." >&2; exit 2; }
            RUNTIME_REQUESTED=true
            APP_PID=$2
            shift 2
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
unset CDPATH ENV BASH_ENV LD_PRELOAD LD_LIBRARY_PATH
umask 077

LOG=/var/lib/webosbrew/chiaki-dualsense-install.log
STATE_DIR=/var/lib/webosbrew/chiaki-dualsense
HOOK_PATH=/var/lib/webosbrew/init.d/90-chiaki-dualsense
REBOOT_MARKER=/tmp/chiaki-dualsense-reboot-required
RUNTIME_STATE_DIR=/var/lib/webosbrew/chiaki-dualsense-runtime
RUNTIME_SOURCE_SHA=a67b5d6fdd8a6350a9398f1afdcdbc95f2a5dc62dfa39d9a86d3095e9b3a577d
UNINSTALL_SOURCE_SHA=aa7f49c002ed228d5cd5ea3d477e22bcc7dad91b60ea9f81b836972f0e3cf831

if [ "$(id -u)" != 0 ]; then
    echo "Homebrew root service is not elevated; leaving the system unchanged." >&2
    exit 77
fi
for parent in /var /var/lib /var/lib/webosbrew; do
    [ -d "$parent" ] && [ ! -L "$parent" ] || exit 77
    [ "$(stat -c '%u:%g:%a' "$parent")" = "0:0:755" ] || exit 77
done
exec >>"$LOG" 2>&1
echo "=== safe compatibility bootstrap $(date) ==="

if [ -z "$BUNDLE_ROOT" ] || [ ! -d "$BUNDLE_ROOT/root" ]; then
    echo "Invalid application bundle: $BUNDLE_ROOT"
    exit 2
fi

valid_decimal()
{
    case "${1:-}" in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac
}

open_verified_source()
{
    source_path=$1
    expected_sha=$2
    fd=$3
    case "$fd" in
        7) exec 7<"$source_path" || return 1 ;;
        8) exec 8<"$source_path" || return 1 ;;
        *) return 1 ;;
    esac
    fd_path=/proc/self/fd/$fd
    [ "$(stat -Lc '%u:%a:%F' "$fd_path" 2>/dev/null)" = \
      "0:755:regular file" ] || return 1
    [ "$(sha256sum "$fd_path" | awk '{print $1}')" = "$expected_sha" ]
}

run_legacy_uninstall()
{
    source_uninstall=$BUNDLE_ROOT/root/uninstall.sh
    if ! open_verified_source "$source_uninstall" "$UNINSTALL_SOURCE_SHA" 7; then
        echo "Bundled legacy cleanup failed source verification."
        return 77
    fi
    /bin/sh /proc/self/fd/7
    status=$?
    exec 7<&-
    return "$status"
}

stage_runtime()
{
    source_runtime=$BUNDLE_ROOT/root/bluetooth-runtime.sh
    if ! open_verified_source "$source_runtime" "$RUNTIME_SOURCE_SHA" 8; then
        echo "Bundled DualSense Bluetooth runtime failed source verification."
        return 77
    fi
    if [ ! -e "$RUNTIME_STATE_DIR" ]; then
        mkdir -m 0700 "$RUNTIME_STATE_DIR"
    fi
    [ -d "$RUNTIME_STATE_DIR" ] && [ ! -L "$RUNTIME_STATE_DIR" ] || return 77
    [ "$(stat -c '%u:%g:%a' "$RUNTIME_STATE_DIR")" = "0:0:700" ] || return 77

    runtime_tmp=$(mktemp "$RUNTIME_STATE_DIR/.runtime.XXXXXX")
    trap 'rm -f "${runtime_tmp:-}"' 0 1 2 15
    dd if=/proc/self/fd/8 of="$runtime_tmp" bs=4096 2>/dev/null
    [ "$(sha256sum "$runtime_tmp" | awk '{print $1}')" = "$RUNTIME_SOURCE_SHA" ] ||
        return 77
    exec 8<&-
    chown root:root "$runtime_tmp"
    chmod 0500 "$runtime_tmp"
    mv -f "$runtime_tmp" "$RUNTIME_STATE_DIR/runtime.sh"
    runtime_tmp=
    sync
    trap - 0 1 2 15
}

cleanup_runtime()
{
    [ -e "$RUNTIME_STATE_DIR" ] || return 0
    stage_runtime || return $?
    /bin/sh "$RUNTIME_STATE_DIR/runtime.sh" cleanup || return $?
    if [ ! -e "$RUNTIME_STATE_DIR/state" ]; then
        rm -f "$RUNTIME_STATE_DIR/runtime.sh" \
              "$RUNTIME_STATE_DIR/watcher.pid" \
              "$RUNTIME_STATE_DIR/watcher.ready" \
              "$RUNTIME_STATE_DIR/lock" \
              "$RUNTIME_STATE_DIR/activation.lock" \
              "$RUNTIME_STATE_DIR/runtime.log"
        rmdir "$RUNTIME_STATE_DIR" 2>/dev/null ||
            echo "Runtime directory contains unknown files; preserving it."
    fi
}

#
# The previously bundled modules have only a generic 4.4.84 vermagic and were
# built from LG source releases that do not match the running firmware kernels.
# Without CONFIG_MODVERSIONS, successful insmod cannot establish core-HID ABI
# compatibility.  Remove an older app-owned installation and fail closed until
# a module has an explicit, exact target-kernel provenance manifest.
#
module_loaded=false
if grep -q '^hid_playstation ' /proc/modules 2>/dev/null; then
    module_loaded=true
fi
if [ -s "$STATE_DIR/hid-playstation.ko" ] && [ "$module_loaded" = true ]; then
    marker_tmp=
    cleanup_marker_tmp() {
        if [ -n "$marker_tmp" ]; then
            rm -f "$marker_tmp"
        fi
    }
    trap cleanup_marker_tmp 0
    trap 'exit 75' 1 2 15
    marker_tmp=$(mktemp "${REBOOT_MARKER}.XXXXXX") || {
        echo "Could not create the legacy-module reboot marker."
        exit 75
    }
    printf '%s\n' "legacy-module-loaded" > "$marker_tmp"
    chmod 0644 "$marker_tmp"
    mv -f "$marker_tmp" "$REBOOT_MARKER"
    marker_tmp=
    trap - 0 1 2 15
elif [ "$module_loaded" = false ]; then
    # Preserve an existing warning until reboot while the module remains loaded.
    rm -f "$REBOOT_MARKER"
fi

if [ -d "$STATE_DIR" ] || [ -e "$HOOK_PATH" ]; then
    echo "Removing legacy unverified DualSense compatibility installation."
    run_legacy_uninstall
else
    rm -f "$HOOK_PATH"
fi

if [ "$module_loaded" = true ]; then
    echo "A PlayStation module is currently loaded; reboot if it came from an older Chiaki build."
fi

if [ "$RUNTIME_REQUESTED" = true ]; then
    valid_decimal "$APP_PID" || { echo "Invalid runtime app PID."; exit 2; }
    [ -r "/proc/$APP_PID/stat" ] || { echo "Runtime app exited."; exit 75; }
    APP_EXE_ID=$(stat -Lc '%d:%i' "/proc/$APP_PID/exe" 2>/dev/null || true)
    BUNDLE_EXE_ID=$(stat -Lc '%d:%i' "$BUNDLE_ROOT/chiaki-webos" \
        2>/dev/null || true)
    [ -n "$APP_EXE_ID" ] && [ "$APP_EXE_ID" = "$BUNDLE_EXE_ID" ] || {
        echo "Runtime PID is not this Chiaki executable."
        exit 77
    }
    [ "$(stat -Lc '%u:%a:%F' "/proc/$APP_PID/exe" 2>/dev/null)" = \
      "0:755:regular file" ] || { echo "Unsafe Chiaki executable identity."; exit 77; }
    [ "$(stat -Lc '%u:%a:%F' "$BUNDLE_ROOT/chiaki-webos" 2>/dev/null)" = \
      "0:755:regular file" ] || { echo "Unsafe bundled Chiaki executable."; exit 77; }
    APP_START=$(awk '{print $22}' "/proc/$APP_PID/stat")
    valid_decimal "$APP_START" || { echo "Invalid runtime app identity."; exit 75; }
    stage_runtime
    /bin/sh "$RUNTIME_STATE_DIR/runtime.sh" prepare \
        "$APP_PID" "$APP_START" "$APP_EXE_ID"
    echo "Experimental DualSense Bluetooth runtime prepared for this app process."
else
    cleanup_runtime
    echo "No ABI-verified compatibility module is bundled; native HID left unchanged."
fi
exit 0
