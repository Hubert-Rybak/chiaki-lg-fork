#!/bin/sh
set -eu

LOG=/tmp/chiaki-hid-playstation.log

exec >>"$LOG" 2>&1
echo "=== denied legacy load $(date) ==="
echo "No exact kernel ABI manifest is installed; refusing patch, module load, and rebind."

# A new package never installs this script as a boot hook.  Do not execute an
# older state-directory uninstaller here: it predates the native-driver guard.
rm -f /var/lib/webosbrew/init.d/90-chiaki-dualsense
exit 77
