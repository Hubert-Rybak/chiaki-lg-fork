#!/bin/sh
set -eu

BUNDLE_ROOT=
REBIND_CONNECTED=false
if [ "$#" -gt 0 ]; then
    BUNDLE_ROOT=$1
    shift
fi
for option in "$@"; do
    case "$option" in
        --rebind-connected) REBIND_CONNECTED=true ;;
        *) echo "Unknown option: $option" >&2; exit 2 ;;
    esac
done
LOG=/tmp/chiaki-hid-playstation-install.log
STATE_DIR=/var/lib/webosbrew/chiaki-dualsense
HOOK_DIR=/var/lib/webosbrew/init.d
HOOK_PATH=$HOOK_DIR/90-chiaki-dualsense

exec >>"$LOG" 2>&1
echo "=== install $(date) ==="

if [ "$(id -u)" != 0 ]; then
    echo "Homebrew root service is not elevated; leaving the system unchanged."
    exit 77
fi
if [ -z "$BUNDLE_ROOT" ] || [ ! -d "$BUNDLE_ROOT/root" ]; then
    echo "Invalid application bundle: $BUNDLE_ROOT"
    exit 2
fi

source_patcher=$BUNDLE_ROOT/root/chiaki-bt-patch
if [ ! -s "$source_patcher" ]; then
    echo "The IPK does not contain root/chiaki-bt-patch"
    exit 3
fi

module_rel=
arch=$(uname -m)
release=$(uname -r)
case "$arch:$release" in
    aarch64:4.4.84*)
        compatible=$(tr '\000' '\n' < /proc/device-tree/compatible 2>/dev/null || true)
        if printf '%s\n' "$compatible" | grep -qx 'lge,lg1212'; then
            module_rel=modules/aarch64/4.4.84/lg1212/hid-playstation.ko
        else
            echo "No ABI-matched module for $arch kernel $release (${compatible:-unknown platform})."
        fi
        ;;
    *)
        echo "No compatibility module for $arch kernel $release."
        ;;
esac
source_module=
if [ -n "$module_rel" ]; then
    source_module=$BUNDLE_ROOT/root/$module_rel
    if [ ! -s "$source_module" ]; then
        echo "The IPK does not contain $module_rel"
        exit 3
    fi

    vermagic=$(modinfo -F vermagic "$source_module" 2>/dev/null || true)
    case "$vermagic" in
        "4.4.84 "*) ;;
        *)
            echo "Refusing incompatible module vermagic: $vermagic"
            exit 4
            ;;
    esac
fi

mkdir -p "$STATE_DIR" "$HOOK_DIR"
cp "$source_patcher" "$STATE_DIR/chiaki-bt-patch.new"
chmod 0755 "$STATE_DIR/chiaki-bt-patch.new"
mv "$STATE_DIR/chiaki-bt-patch.new" "$STATE_DIR/chiaki-bt-patch"
cp "$BUNDLE_ROOT/root/load.sh" "$STATE_DIR/load.sh"
chmod 0755 "$STATE_DIR/load.sh"
cp "$BUNDLE_ROOT/root/uninstall.sh" "$STATE_DIR/uninstall.sh"
chmod 0755 "$STATE_DIR/uninstall.sh"

if [ -n "$source_module" ]; then
    cp "$source_module" "$STATE_DIR/hid-playstation.ko.new"
    chmod 0644 "$STATE_DIR/hid-playstation.ko.new"
    mv "$STATE_DIR/hid-playstation.ko.new" "$STATE_DIR/hid-playstation.ko"
else
    rm -f "$STATE_DIR/hid-playstation.ko"
fi

set +e
if [ "$REBIND_CONNECTED" = true ]; then
    "$STATE_DIR/load.sh" --rebind
else
    "$STATE_DIR/load.sh"
fi
load_status=$?
set -e
case "$load_status" in
    0|75)
        # PID 75 means the Bluetooth daemon was not up yet. The boot hook and
        # the next app launch retry without touching the library on disk.
        if [ "$REBIND_CONNECTED" = true ]; then
            echo "Connected compatible controllers were rebound before SDL startup."
        else
            echo "Connected controllers were left untouched."
        fi
        cp "$BUNDLE_ROOT/root/boot-hook.sh" "$HOOK_PATH"
        chmod 0755 "$HOOK_PATH"
        ;;
    77)
        echo "No compatible kernel module or Bluetooth runtime patch; leaving no boot hook."
        rm -f "$HOOK_PATH"
        rm -rf "$STATE_DIR"
        ;;
    *)
        echo "Compatibility activation failed with status $load_status."
        exit "$load_status"
        ;;
esac
