#!/bin/sh
set -eu

STATE_DIR=/var/lib/webosbrew/chiaki-dualsense
HOOK=/var/lib/webosbrew/init.d/90-chiaki-dualsense
PATCH_MARKER=/tmp/chiaki-bluetooth-output-patched

if [ "$(id -u)" != 0 ]; then
    echo "Root is required."
    exit 77
fi

# Never unbind or unload by module name here: an LG-provided driver may have the
# same name.  Removing the app-owned boot hook and payload prevents a legacy
# module from returning after reboot without touching a native driver.
if [ -s "$STATE_DIR/hid-playstation.ko" ] &&
   grep -q '^hid_playstation ' /proc/modules 2>/dev/null; then
    echo "Legacy module may remain active until the TV is rebooted."
fi

# Disable persistence before attempting a live-memory rollback.  If the exact
# patched daemon is still running, retain the helper and marker until restore
# succeeds so the next app launch can retry safely.
rm -f "$HOOK"
if [ -e "$PATCH_MARKER" ] || [ -L "$PATCH_MARKER" ]; then
    patched_pid=$(sed -n '1p' "$PATCH_MARKER" 2>/dev/null || true)
    bluetooth_pids=" $(pidof webos-bluetooth-service 2>/dev/null || true) "
    case "$patched_pid" in
        ''|*[!0-9]*)
            echo "Invalid legacy patch marker; retaining rollback state."
            exit 75
            ;;
        *)
            case "$bluetooth_pids" in
                *" $patched_pid "*)
                    if [ ! -x "$STATE_DIR/chiaki-bt-patch" ] ||
                       ! "$STATE_DIR/chiaki-bt-patch" "$patched_pid" --restore; then
                        echo "Live Bluetooth rollback failed; retaining helper for retry."
                        exit 75
                    fi
                    ;;
                *)
                    echo "Previously patched Bluetooth process is no longer running."
                    ;;
            esac
            ;;
    esac
fi

rm -f "$PATCH_MARKER"
rm -f "$STATE_DIR/chiaki-bt-patch"
rm -f "$STATE_DIR/chiaki-bt-patch.new"
rm -f "$STATE_DIR/hid-playstation.ko"
rm -f "$STATE_DIR/hid-playstation.ko.new"
rm -f "$STATE_DIR/load.sh"
rm -f "$STATE_DIR/uninstall.sh"
if [ -d "$STATE_DIR" ] && ! rmdir "$STATE_DIR" 2>/dev/null; then
    echo "Legacy state contains unknown files; known app payloads were removed."
fi
