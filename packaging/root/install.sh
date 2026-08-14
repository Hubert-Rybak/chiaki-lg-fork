#!/bin/sh
set -eu

BUNDLE_ROOT=${1:-}
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

arch=$(uname -m)
release=$(uname -r)
case "$arch:$release" in
    aarch64:4.4.84*) module_rel=modules/aarch64/4.4.84/hid-playstation.ko ;;
    *)
        echo "No compatibility module for $arch kernel $release; leaving the system unchanged."
        exit 0
        ;;
esac

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

mkdir -p "$STATE_DIR" "$HOOK_DIR"
cp "$source_module" "$STATE_DIR/hid-playstation.ko.new"
chmod 0644 "$STATE_DIR/hid-playstation.ko.new"
mv "$STATE_DIR/hid-playstation.ko.new" "$STATE_DIR/hid-playstation.ko"
cp "$BUNDLE_ROOT/root/load.sh" "$STATE_DIR/load.sh"
chmod 0755 "$STATE_DIR/load.sh"
cp "$BUNDLE_ROOT/root/uninstall.sh" "$STATE_DIR/uninstall.sh"
chmod 0755 "$STATE_DIR/uninstall.sh"

"$STATE_DIR/load.sh" --rebind

# Persist only after the module has loaded and any connected controller has
# successfully rebound. A failed first test therefore cannot create a boot loop.
cp "$BUNDLE_ROOT/root/boot-hook.sh" "$HOOK_PATH"
chmod 0755 "$HOOK_PATH"
