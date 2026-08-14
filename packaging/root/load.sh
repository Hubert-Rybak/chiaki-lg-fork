#!/bin/sh
set -eu

STATE_DIR=/var/lib/webosbrew/chiaki-dualsense
MODULE=$STATE_DIR/hid-playstation.ko
LOG=/tmp/chiaki-hid-playstation.log
REBIND=${1:-}

exec >>"$LOG" 2>&1
echo "=== load $(date) ==="

if [ "$(id -u)" != 0 ]; then
    echo "Root is required."
    exit 77
fi
if [ -d /sys/bus/hid/drivers/playstation ]; then
    echo "playstation HID driver is already registered."
else
    if [ ! -s "$MODULE" ]; then
        echo "Missing module: $MODULE"
        exit 2
    fi
    insmod "$MODULE"
    echo "Loaded $MODULE"
fi

[ "$REBIND" = "--rebind" ] || exit 0

for device in /sys/bus/hid/devices/*; do
    [ -r "$device/uevent" ] || continue
    if ! grep -Eq '^HID_ID=....:0000054C:00000(CE6|DF2)$' "$device/uevent"; then
        continue
    fi

    driver=$(readlink "$device/driver" 2>/dev/null || true)
    case "$driver" in
        */playstation)
            echo "${device##*/} is already bound to playstation."
            ;;
        */hid-generic)
            id=${device##*/}
            echo "$id" > /sys/bus/hid/drivers/hid-generic/unbind
            if echo "$id" > /sys/bus/hid/drivers/playstation/bind; then
                echo "Rebound $id to playstation."
            else
                echo "playstation bind failed for $id; restoring hid-generic."
                echo "$id" > /sys/bus/hid/drivers/hid-generic/bind || true
                exit 5
            fi
            ;;
        *)
            echo "Leaving ${device##*/} bound to ${driver:-none}."
            ;;
    esac
done
