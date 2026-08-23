#!/bin/sh
set -eu

BUNDLE_ROOT=
if [ "$#" -gt 0 ]; then
    BUNDLE_ROOT=$1
    shift
fi
for option in "$@"; do
    case "$option" in
        --rebind-connected) : ;; # accepted for older app binaries
        *) echo "Unknown option: $option" >&2; exit 2 ;;
    esac
done

LOG=/tmp/chiaki-hid-playstation-install.log
STATE_DIR=/var/lib/webosbrew/chiaki-dualsense
HOOK_PATH=/var/lib/webosbrew/init.d/90-chiaki-dualsense
REBOOT_MARKER=/tmp/chiaki-dualsense-reboot-required

exec >>"$LOG" 2>&1
echo "=== safe cleanup $(date) ==="

if [ "$(id -u)" != 0 ]; then
    echo "Homebrew root service is not elevated; leaving the system unchanged."
    exit 77
fi
if [ -z "$BUNDLE_ROOT" ] || [ ! -d "$BUNDLE_ROOT/root" ]; then
    echo "Invalid application bundle: $BUNDLE_ROOT"
    exit 2
fi

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
    /bin/sh "$BUNDLE_ROOT/root/uninstall.sh"
else
    rm -f "$HOOK_PATH"
fi

if [ "$module_loaded" = true ]; then
    echo "A PlayStation module is currently loaded; reboot if it came from an older Chiaki build."
fi

echo "No ABI-verified compatibility module is bundled; native HID left unchanged."
exit 0
