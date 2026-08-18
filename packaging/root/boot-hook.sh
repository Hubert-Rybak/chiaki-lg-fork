#!/bin/sh
attempt=0
while [ "$attempt" -lt 10 ]; do
    set +e
    /var/lib/webosbrew/chiaki-dualsense/load.sh
    status=$?
    set -e
    case "$status" in
        0) exit 0 ;;
        75)
            attempt=$((attempt + 1))
            sleep 1
            ;;
        77) exit 0 ;;
        *) exit "$status" ;;
    esac
done
exit 0
