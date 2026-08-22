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

is_dualsense_device() {
    grep -Eq '^HID_ID=....:0000054C:00000(CE6|DF2)$' "$1/uevent"
}

device_has_input_node() {
    for event in "$1"/input/input*/event*; do
        [ -e "$event" ] || continue
        [ -e "/dev/input/${event##*/}" ] && return 0
    done
    return 1
}

restore_compat_devices() {
    restore_failed=false
    for device in /sys/bus/hid/devices/*; do
        [ -r "$device/uevent" ] || continue
        is_dualsense_device "$device" || continue
        driver=$(readlink "$device/driver" 2>/dev/null || true)
        case "$driver" in */playstation) ;; *) continue ;; esac

        id=${device##*/}
        echo "Restoring $id to hid-generic because Bluetooth correction is inactive."
        if ! echo "$id" > /sys/bus/hid/drivers/playstation/unbind; then
            echo "Could not unbind $id from playstation."
            restore_failed=true
            continue
        fi
        if ! echo "$id" > /sys/bus/hid/drivers/hid-generic/bind; then
            echo "Could not restore $id to hid-generic."
            restore_failed=true
        fi
    done

    if grep -q '^hid_playstation ' /proc/modules 2>/dev/null; then
        if rmmod hid_playstation; then
            echo "Unloaded compatibility driver while Bluetooth correction is inactive."
        else
            echo "Could not unload compatibility driver."
            restore_failed=true
        fi
    fi
    [ "$restore_failed" = false ]
}

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

# The compatibility driver claims Bluetooth devices as soon as it is loaded and
# sends an initialization report from probe. Never load or rebind it unless the
# LG Bluetooth output-report correction is confirmed active. Basic controller
# input remains available through hid-generic on unsupported firmware.
if [ "$patch_status" != 0 ]; then
    rm -f "$PATCH_MARKER"
    if [ -s "$MODULE" ] && ! restore_compat_devices; then
        echo "Could not restore the safe hid-generic fallback."
        exit 6
    fi
    case "$patch_status" in
        75)
            echo "Bluetooth correction is temporarily unavailable; compatibility activation deferred."
            exit 75
            ;;
        77)
            echo "Bluetooth correction is unsupported; compatibility driver left disabled."
            exit 77
            ;;
        *)
            echo "Bluetooth correction failed with status $patch_status; compatibility driver left disabled."
            exit "$patch_status"
            ;;
    esac
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
        is_dualsense_device "$device" || continue

        driver=$(readlink "$device/driver" 2>/dev/null || true)
        case "$driver" in
            */playstation)
                if device_has_input_node "$device"; then
                    echo "${device##*/} is already bound to playstation with an input node."
                else
                    id=${device##*/}
                    echo "$id" > /sys/bus/hid/drivers/playstation/unbind || true
                    echo "$id" > /sys/bus/hid/drivers/hid-generic/bind || true
                    echo "playstation has no input node for $id; restored hid-generic."
                    restore_compat_devices || exit 6
                    exit 5
                fi
                ;;
            */hid-generic)
                id=${device##*/}
                echo "$id" > /sys/bus/hid/drivers/hid-generic/unbind
                if echo "$id" > /sys/bus/hid/drivers/playstation/bind; then
                    rebound_driver=$(readlink "$device/driver" 2>/dev/null || true)
                    case "$rebound_driver" in
                        */playstation)
                            if ! device_has_input_node "$device"; then
                                sleep 1
                            fi
                            if device_has_input_node "$device"; then
                                echo "Rebound $id to playstation with a usable input node."
                            else
                                echo "$id" > /sys/bus/hid/drivers/playstation/unbind || true
                                echo "$id" > /sys/bus/hid/drivers/hid-generic/bind || true
                                echo "playstation created no input node for $id; restored hid-generic."
                                restore_compat_devices || exit 6
                                exit 5
                            fi
                            ;;
                        *)
                            echo "playstation did not claim $id; restoring hid-generic."
                            echo "$id" > /sys/bus/hid/drivers/hid-generic/bind || true
                            restore_compat_devices || exit 6
                            exit 5
                            ;;
                    esac
                else
                    echo "playstation bind failed for $id; restoring hid-generic."
                    echo "$id" > /sys/bus/hid/drivers/hid-generic/bind || true
                    restore_compat_devices || exit 6
                    exit 5
                fi
                ;;
            *)
                echo "Leaving ${device##*/} bound to ${driver:-none}."
                ;;
        esac
    done
fi

if [ "$module_available" = true ] || [ "$patch_status" = 0 ]; then
    exit 0
fi
exit 77
