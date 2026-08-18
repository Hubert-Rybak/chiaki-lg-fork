#!/bin/sh
set -eu

STATE_DIR=/var/lib/webosbrew/chiaki-dualsense
HOOK=/var/lib/webosbrew/init.d/90-chiaki-dualsense
PATCH_MARKER=/tmp/chiaki-bluetooth-output-patched

if [ "$(id -u)" != 0 ]; then
    echo "Root is required."
    exit 77
fi

for device in /sys/bus/hid/devices/*; do
    [ -r "$device/uevent" ] || continue
    driver=$(readlink "$device/driver" 2>/dev/null || true)
    case "$driver" in */playstation) ;; *) continue ;; esac
    id=${device##*/}
    echo "$id" > /sys/bus/hid/drivers/playstation/unbind
    echo "$id" > /sys/bus/hid/drivers/hid-generic/bind || true
done

rmmod hid_playstation 2>/dev/null || true

if [ -x "$STATE_DIR/chiaki-bt-patch" ]; then
    bluetooth_pid=$(pidof webos-bluetooth-service 2>/dev/null || true)
    case "$bluetooth_pid" in
        ''|*' '*) ;;
        *) "$STATE_DIR/chiaki-bt-patch" "$bluetooth_pid" --restore || true ;;
    esac
fi

rm -f "$PATCH_MARKER"
rm -f "$HOOK"
rm -rf "$STATE_DIR"
