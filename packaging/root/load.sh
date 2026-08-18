#!/bin/sh
set -eu

STATE_DIR=/var/lib/webosbrew/chiaki-dualsense
MODULE=$STATE_DIR/hid-playstation.ko
PATCHER=$STATE_DIR/chiaki-bt-patch
PATCH_MARKER=/tmp/chiaki-bluetooth-output-patched
LOG=/tmp/chiaki-hid-playstation.log
REBIND=false
SKIP_PATCH=false

for option in "$@"; do
    case "$option" in
        --rebind) REBIND=true ;;
        --skip-bluetooth-patch) SKIP_PATCH=true ;;
        *) echo "Unknown option: $option" >&2; exit 2 ;;
    esac
done

exec >>"$LOG" 2>&1
echo "=== load $(date) ==="

if [ "$(id -u)" != 0 ]; then
    echo "Root is required."
    exit 77
fi

patch_status=77
if [ "$SKIP_PATCH" = false ] && [ -x "$PATCHER" ]; then
    bluetooth_pid=$(pidof webos-bluetooth-service 2>/dev/null || true)
    if [ -z "$bluetooth_pid" ]; then
        echo "Bluetooth service is not running yet; runtime correction deferred."
        patch_status=75
    else
        # pidof should return one daemon. Refuse an ambiguous process list.
        case "$bluetooth_pid" in *' '*)
            echo "Multiple Bluetooth service PIDs found: $bluetooth_pid"
            patch_status=77
            ;;
        *)
            set +e
            "$PATCHER" "$bluetooth_pid"
            patch_status=$?
            set -e
            if [ "$patch_status" = 0 ]; then
                printf '%s\n' "$bluetooth_pid" > "$PATCH_MARKER"
                chmod 0644 "$PATCH_MARKER"
            else
                rm -f "$PATCH_MARKER"
            fi
            ;;
        esac
    fi
fi

module_available=false
if [ -s "$MODULE" ]; then
    module_available=true
    if [ -d /sys/bus/hid/drivers/playstation ]; then
        echo "playstation HID driver is already registered."
    else
        insmod "$MODULE"
        echo "Loaded $MODULE"
    fi
fi

if [ "$REBIND" = true ] && [ "$module_available" = true ]; then
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
                    rebound_driver=$(readlink "$device/driver" 2>/dev/null || true)
                    case "$rebound_driver" in
                        */playstation)
                            echo "Rebound $id to playstation."
                            ;;
                        *)
                            echo "playstation did not claim $id; restoring hid-generic."
                            echo "$id" > /sys/bus/hid/drivers/hid-generic/bind || true
                            exit 5
                            ;;
                    esac
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
fi

if [ "$patch_status" = 75 ]; then
    exit 75
fi
if [ "$module_available" = true ] || [ "$patch_status" = 0 ]; then
    exit 0
fi
exit 77
