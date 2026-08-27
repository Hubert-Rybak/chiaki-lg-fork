#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
unset CDPATH ENV BASH_ENV LD_PRELOAD LD_LIBRARY_PATH
umask 077

# Root-only, opt-in DualSense Bluetooth session for the one LG firmware that
# has been hardware-verified.  Both changes are volatile: configd keeps the
# allowlist in memory, and the daemon correction changes one instruction byte
# in the running process.  Nothing is installed at boot and no kernel module is
# loaded.

STATE_DIR=/var/lib/webosbrew/chiaki-dualsense-runtime
STATE_FILE=$STATE_DIR/state
LOCK_FILE=$STATE_DIR/lock
ACTIVATION_LOCK_FILE=$STATE_DIR/activation.lock
READY_FILE=$STATE_DIR/watcher.ready
LOG=$STATE_DIR/runtime.log
KEY=com.webos.service.bthidmanager.supportedBtGamepadList
OWNER_KEY=org.webosbrew.chiaki.dualsenseSessionOwner
CONFIG_URI=luna://com.webos.service.config
BASE_CONFIG=/etc/configd/layers/base/com.webos.service.bthidmanager.json
BASE_CONFIG_SHA=9b8c9172c8df0dbc539921c3de63a41e84af063786f5feecf1502b95ba6bceff
BT_LIBRARY=/usr/lib/libbluetooth.default.so
BT_LIBRARY_SHA=e0beb5a886d37e5358bf3324a60a207d45a703e87015cdceddfe265ca5e6965f
PATCH_FILE_OFFSET=$((0x82afa))
SIGNATURE_OFF=bb680322da80bb691b79ba68f9691846
SIGNATURE_ON=bb680222da80bb691b79ba68f9691846

BASE_SET_ARRAY='[{"name":"Xbox Wireless Controller","vid":1118,"pid":765},{"name":"Xbox Wireless Controller 987A","vid":1118,"pid":2835},{"name":"Wireless Controller","vid":1356,"pid":1476},{"name":"Wireless Controller","vid":1356,"pid":2508},{"name":"NVIDIA Controller v01.04","vid":2389,"pid":29204},{"name":"Luna Gamepad","vid":369,"pid":1049},{"name":"Bluetooth Game Controller","vid":1118,"pid":765},{"name":"Bluetooth Game Controller","vid":1118,"pid":2835},{"name":"Bluetooth Game Controller","vid":1356,"pid":1476},{"name":"Bluetooth Game Controller","vid":1356,"pid":2508},{"name":"Bluetooth Game Controller","vid":2389,"pid":29204},{"name":"Bluetooth Game Controller","vid":369,"pid":1049}]'
OURS_SET_ARRAY='[{"name":"Xbox Wireless Controller","vid":1118,"pid":765},{"name":"Xbox Wireless Controller 987A","vid":1118,"pid":2835},{"name":"Wireless Controller","vid":1356,"pid":1476},{"name":"Wireless Controller","vid":1356,"pid":2508},{"name":"NVIDIA Controller v01.04","vid":2389,"pid":29204},{"name":"Luna Gamepad","vid":369,"pid":1049},{"name":"Bluetooth Game Controller","vid":1118,"pid":765},{"name":"Bluetooth Game Controller","vid":1118,"pid":2835},{"name":"Bluetooth Game Controller","vid":1356,"pid":1476},{"name":"Bluetooth Game Controller","vid":1356,"pid":2508},{"name":"Bluetooth Game Controller","vid":2389,"pid":29204},{"name":"Bluetooth Game Controller","vid":369,"pid":1049},{"name":"DualSense Wireless Controller","vid":1356,"pid":3302}]'
BASE_QUERY_ARRAY='[{"vid":1118,"pid":765,"name":"Xbox Wireless Controller"},{"vid":1118,"pid":2835,"name":"Xbox Wireless Controller 987A"},{"vid":1356,"pid":1476,"name":"Wireless Controller"},{"vid":1356,"pid":2508,"name":"Wireless Controller"},{"vid":2389,"pid":29204,"name":"NVIDIA Controller v01.04"},{"vid":369,"pid":1049,"name":"Luna Gamepad"},{"vid":1118,"pid":765,"name":"Bluetooth Game Controller"},{"vid":1118,"pid":2835,"name":"Bluetooth Game Controller"},{"vid":1356,"pid":1476,"name":"Bluetooth Game Controller"},{"vid":1356,"pid":2508,"name":"Bluetooth Game Controller"},{"vid":2389,"pid":29204,"name":"Bluetooth Game Controller"},{"vid":369,"pid":1049,"name":"Bluetooth Game Controller"}]'
OURS_QUERY_ARRAY='[{"vid":1118,"pid":765,"name":"Xbox Wireless Controller"},{"vid":1118,"pid":2835,"name":"Xbox Wireless Controller 987A"},{"vid":1356,"pid":1476,"name":"Wireless Controller"},{"vid":1356,"pid":2508,"name":"Wireless Controller"},{"vid":2389,"pid":29204,"name":"NVIDIA Controller v01.04"},{"vid":369,"pid":1049,"name":"Luna Gamepad"},{"vid":1118,"pid":765,"name":"Bluetooth Game Controller"},{"vid":1118,"pid":2835,"name":"Bluetooth Game Controller"},{"vid":1356,"pid":1476,"name":"Bluetooth Game Controller"},{"vid":1356,"pid":2508,"name":"Bluetooth Game Controller"},{"vid":2389,"pid":29204,"name":"Bluetooth Game Controller"},{"vid":369,"pid":1049,"name":"Bluetooth Game Controller"},{"vid":1356,"pid":3302,"name":"DualSense Wireless Controller"}]'

log()
{
    printf '%s\n' "$*" >> "$LOG"
}

valid_decimal()
{
    case "${1:-}" in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac
}

valid_devino()
{
    case "${1:-}" in
        *:*) left=${1%%:*}; right=${1#*:} ;;
        *) return 1 ;;
    esac
    valid_decimal "$left" && valid_decimal "$right"
}

valid_boot_id()
{
    value=${1:-}
    [ "${#value}" = 36 ] || return 1
    case "$value" in *[!0-9a-f-]*) return 1 ;; *) return 0 ;; esac
}

process_matches()
{
    pid=$1
    start=$2
    valid_decimal "$pid" && valid_decimal "$start" &&
        [ -r "/proc/$pid/stat" ] &&
        [ "$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null)" = "$start" ]
}

app_matches()
{
    pid=$1
    start=$2
    exe_id=$3
    process_matches "$pid" "$start" &&
        [ "$(stat -Lc '%d:%i' "/proc/$pid/exe" 2>/dev/null)" = "$exe_id" ]
}

daemon_matches()
{
    pid=$1
    start=$2
    exe_id=$3
    process_matches "$pid" "$start" &&
        [ "$(readlink -f "/proc/$pid/exe" 2>/dev/null)" = \
          "/usr/sbin/webos-bluetooth-service" ] &&
        [ "$(stat -Lc '%d:%i' "/proc/$pid/exe" 2>/dev/null)" = "$exe_id" ]
}

get_runtime_config()
{
    payload='{"configNames":["'"$KEY"'","'"$OWNER_KEY"'"]}'
    /usr/bin/luna-send -n 1 -w 1500 "$CONFIG_URI/getConfigs" "$payload"
}

runtime_config_is()
{
    expected=$1
    expected_owner=$2
    response=$(get_runtime_config 2>/dev/null || true)
    case "$response" in *'"returnValue":true'*) ;; *) return 1 ;; esac
    case "$response" in *'"'"$KEY"'":'"$expected"*) ;; *) return 1 ;; esac
    case "$response" in
        *'"'"$OWNER_KEY"'":"'"$expected_owner"'"'*) return 0 ;;
        *) return 1 ;;
    esac
}

baseline_config_is()
{
    response=$(get_runtime_config 2>/dev/null || true)
    case "$response" in *'"returnValue":true'*) ;; *) return 1 ;; esac
    case "$response" in *'"'"$KEY"'":'"$BASE_QUERY_ARRAY"*) ;; *) return 1 ;; esac
    case "$response" in
        *'"'"$OWNER_KEY"'":""'*) return 0 ;;
        *'"'"$OWNER_KEY"'":'*) return 1 ;;
        *'"missingConfigs":'*'"'"$OWNER_KEY"'"'*) return 0 ;;
        *) return 1 ;;
    esac
}

owner_is()
{
    expected_owner=$1
    response=$(get_runtime_config 2>/dev/null || true)
    case "$response" in *'"returnValue":true'*) ;; *) return 1 ;; esac
    case "$response" in
        *'"'"$OWNER_KEY"'":"'"$expected_owner"'"'*) return 0 ;;
        *) return 1 ;;
    esac
}

allowlist_value_is()
{
    expected=$1
    response=$(get_runtime_config 2>/dev/null || true)
    case "$response" in *'"returnValue":true'*) ;; *) return 1 ;; esac
    case "$response" in
        *'"'"$KEY"'":'"$expected"*) return 0 ;;
        *) return 1 ;;
    esac
}

set_runtime_config()
{
    array=$1
    owner=$2
    payload='{"configs":{"'"$KEY"'":'"$array"',"'"$OWNER_KEY"'":"'"$owner"'"},"volatile":true}'
    response=$(/usr/bin/luna-send -n 1 -w 1500 \
        "$CONFIG_URI/setConfigs" "$payload" 2>/dev/null || true)
    case "$response" in *'"returnValue":true'*) return 0 ;; *) return 1 ;; esac
}

clear_allowlist()
{
    payload='{"configs":{"'"$KEY"'":null},"volatile":true}'
    response=$(/usr/bin/luna-send -n 1 -w 1500 \
        "$CONFIG_URI/setConfigs" "$payload" 2>/dev/null || true)
    case "$response" in *'"returnValue":true'*) return 0 ;; *) return 1 ;; esac
}

reset_owner()
{
    # This LG configd build reports success for null on an app-defined
    # volatile key but leaves the old value in memory. An empty string is the
    # verified neutral value; it is harmless and disappears on reboot.
    payload='{"configs":{"'"$OWNER_KEY"'":""},"volatile":true}'
    response=$(/usr/bin/luna-send -n 1 -w 1500 \
        "$CONFIG_URI/setConfigs" "$payload" 2>/dev/null || true)
    case "$response" in *'"returnValue":true'*) return 0 ;; *) return 1 ;; esac
}

clear_owned_runtime_config()
{
    # LG configd accepts both keys in one SET but, on this firmware, a
    # multi-key null can clear the list while leaving the owner behind. Clear
    # in a verified order that never loses ownership before the global list is
    # back at its tested base value.
    clear_allowlist || return 1
    allowlist_value_is "$BASE_QUERY_ARRAY" || return 1
    owner_is "$OWNER_NONCE" || return 1
    reset_owner || return 1
    baseline_config_is
}

find_patch_address()
{
    target_pid=$1
    PATCH_ADDRESS=
    while read -r range permissions offset device inode path; do
        [ "$path" = "$BT_LIBRARY" ] || continue
        start_hex=${range%-*}
        end_hex=${range#*-}
        start=$((0x$start_hex))
        end=$((0x$end_hex))
        map_offset=$((0x$offset))
        candidate=$((start + PATCH_FILE_OFFSET - map_offset))
        if [ "$candidate" -ge "$start" ] && [ "$candidate" -lt "$end" ]; then
            PATCH_ADDRESS=$candidate
            break
        fi
    done < "/proc/$target_pid/maps"
    [ -n "$PATCH_ADDRESS" ]
}

read_signature()
{
    target_pid=$1
    patch_address=$2
    signature_address=$((patch_address - 2))
    dd if="/proc/$target_pid/mem" bs=1 skip="$signature_address" count=16 \
        2>/dev/null | od -An -tx1 | tr -d ' \n'
}

write_patch_byte()
{
    target_pid=$1
    patch_address=$2
    octal_byte=$3
    # shellcheck disable=SC2059 -- the caller supplies a fixed octal literal.
    printf "$octal_byte" | dd of="/proc/$target_pid/mem" bs=1 \
        seek="$patch_address" count=1 conv=notrunc 2>/dev/null
}

discover_daemon()
{
    DAEMON_PID=$(pidof webos-bluetooth-service 2>/dev/null || true)
    case "$DAEMON_PID" in ''|*' '*) return 1 ;; esac
    [ "$(stat -c %u "/proc/$DAEMON_PID" 2>/dev/null)" = 0 ] || return 1
    DAEMON_START=$(awk '{print $22}' "/proc/$DAEMON_PID/stat")
    [ "$(readlink -f "/proc/$DAEMON_PID/exe" 2>/dev/null)" = \
      "/usr/sbin/webos-bluetooth-service" ] || return 1
    DAEMON_EXE_ID=$(stat -Lc '%d:%i' "/proc/$DAEMON_PID/exe")
    [ "$(sha256sum "$BT_LIBRARY" | awk '{print $1}')" = "$BT_LIBRARY_SHA" ] ||
        return 1
    find_patch_address "$DAEMON_PID"
}

prepare_patch_target()
{
    discover_daemon || return 1
    [ "$(read_signature "$DAEMON_PID" "$PATCH_ADDRESS")" = "$SIGNATURE_OFF" ] ||
        return 1
}

enable_patch()
{
    daemon_matches "$DAEMON_PID" "$DAEMON_START" "$DAEMON_EXE_ID" || return 1
    [ "$(read_signature "$DAEMON_PID" "$PATCH_ADDRESS")" = "$SIGNATURE_OFF" ] ||
        return 1
    write_patch_byte "$DAEMON_PID" "$PATCH_ADDRESS" '\002'
    [ "$(read_signature "$DAEMON_PID" "$PATCH_ADDRESS")" = "$SIGNATURE_ON" ]
}

restore_patch()
{
    target_pid=$1
    target_start=$2
    target_address=$3
    target_exe_id=$4
    target_boot_id=$5
    if [ "$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)" != \
         "$target_boot_id" ]; then
        log "TV rebooted; both volatile compatibility changes vanished."
        return 0
    fi
    if ! daemon_matches "$target_pid" "$target_start" "$target_exe_id"; then
        log "Bluetooth daemon exited; the memory-only correction vanished with it."
        return 0
    fi
    if [ "$(sha256sum "$BT_LIBRARY" | awk '{print $1}')" != "$BT_LIBRARY_SHA" ]; then
        log "Bluetooth library changed; refusing an unsafe restore."
        return 1
    fi
    current=$(read_signature "$target_pid" "$target_address" || true)
    if [ "$current" = "$SIGNATURE_OFF" ]; then
        return 0
    fi
    if [ "$current" != "$SIGNATURE_ON" ]; then
        log "Bluetooth correction signature drifted; refusing an unsafe restore."
        return 1
    fi
    write_patch_byte "$target_pid" "$target_address" '\003'
    [ "$(read_signature "$target_pid" "$target_address")" = "$SIGNATURE_OFF" ]
}

write_state()
{
    phase=$1
    temporary=$(mktemp "$STATE_DIR/.state.XXXXXX")
    trap 'rm -f "$temporary"' 0 1 2 15
    {
        printf 'PHASE=%s\n' "$phase"
        printf 'APP_PID=%s\n' "$APP_PID"
        printf 'APP_START=%s\n' "$APP_START"
        printf 'APP_EXE_ID=%s\n' "$APP_EXE_ID"
        printf 'BOOT_ID=%s\n' "$BOOT_ID"
        printf 'DAEMON_PID=%s\n' "${DAEMON_PID:-0}"
        printf 'DAEMON_START=%s\n' "${DAEMON_START:-0}"
        printf 'DAEMON_EXE_ID=%s\n' "${DAEMON_EXE_ID:-0:0}"
        printf 'PATCH_ADDRESS=%s\n' "${PATCH_ADDRESS:-0}"
        printf 'OWNER_NONCE=%s\n' "${OWNER_NONCE:-00000000000000000000000000000000}"
    } > "$temporary"
    chmod 0600 "$temporary"
    chown root:root "$temporary"
    mv -f "$temporary" "$STATE_FILE"
    sync
    temporary=
    trap - 0 1 2 15
}

read_state_value()
{
    field=$1
    count=$(grep -c "^${field}=" "$STATE_FILE" 2>/dev/null || true)
    [ "$count" = 1 ] || return 1
    sed -n "s/^${field}=//p" "$STATE_FILE"
}

load_state()
{
    [ -f "$STATE_FILE" ] && [ ! -L "$STATE_FILE" ] || return 1
    [ "$(stat -c '%u:%g:%a:%h' "$STATE_FILE")" = "0:0:600:1" ] || return 1
    PHASE=$(read_state_value PHASE) || return 1
    APP_PID=$(read_state_value APP_PID) || return 1
    APP_START=$(read_state_value APP_START) || return 1
    APP_EXE_ID=$(read_state_value APP_EXE_ID) || return 1
    BOOT_ID=$(read_state_value BOOT_ID) || return 1
    DAEMON_PID=$(read_state_value DAEMON_PID) || return 1
    DAEMON_START=$(read_state_value DAEMON_START) || return 1
    DAEMON_EXE_ID=$(read_state_value DAEMON_EXE_ID) || return 1
    PATCH_ADDRESS=$(read_state_value PATCH_ADDRESS) || return 1
    OWNER_NONCE=$(read_state_value OWNER_NONCE) || return 1
    case "$PHASE" in
        PREPARED|ALLOWLIST_INTENT|ALLOWLIST_ACTIVE|PATCH_INTENT|ACTIVE) ;;
        *) return 1 ;;
    esac
    valid_decimal "$APP_PID" && valid_decimal "$APP_START" &&
        valid_devino "$APP_EXE_ID" && valid_boot_id "$BOOT_ID" &&
        valid_decimal "$DAEMON_PID" && valid_decimal "$DAEMON_START" &&
        valid_devino "$DAEMON_EXE_ID" &&
        valid_decimal "$PATCH_ADDRESS" &&
        [ "${#OWNER_NONCE}" = 32 ] &&
        case "$OWNER_NONCE" in *[!0-9a-f]*) false ;; *) true ;; esac
}

cleanup_session()
{
    load_state || {
        log "Runtime state is missing or invalid; refusing guessed cleanup."
        return 1
    }
    cleanup_ok=true
    case "$PHASE" in
    PATCH_INTENT|ACTIVE)
        if ! restore_patch "$DAEMON_PID" "$DAEMON_START" "$PATCH_ADDRESS" \
                              "$DAEMON_EXE_ID" "$BOOT_ID"; then
            cleanup_ok=false
        fi
        ;;
    esac
    case "$PHASE" in
    ALLOWLIST_INTENT|ALLOWLIST_ACTIVE|PATCH_INTENT|ACTIVE)
        if runtime_config_is "$OURS_QUERY_ARRAY" "$OWNER_NONCE"; then
            if ! clear_owned_runtime_config; then
                log "Could not verify volatile allowlist rollback."
                cleanup_ok=false
            fi
        elif baseline_config_is; then
            if [ "$PHASE" = ALLOWLIST_INTENT ]; then
                # A timed-out client may have left a request queued. Require a
                # second stable observation before discarding the intent WAL.
                sleep 2
                baseline_config_is || cleanup_ok=false
            fi
        elif owner_is "$OWNER_NONCE"; then
            # Another actor changed the whole-array value. Relinquish only our
            # companion marker and never overwrite that actor's list.
            if ! reset_owner; then
                log "Could not remove the drifted runtime owner marker."
            fi
            if allowlist_value_is "$BASE_QUERY_ARRAY" && baseline_config_is; then
                log "Partial allowlist transaction relinquished safely."
            else
                log "Allowlist ownership drifted; refusing to overwrite it."
                cleanup_ok=false
            fi
        else
            log "Allowlist ownership is ambiguous; refusing to overwrite it."
            cleanup_ok=false
        fi
        ;;
    esac
    if [ "$cleanup_ok" = true ]; then
        rm -f "$STATE_FILE" "$READY_FILE" "$STATE_DIR/watcher.pid"
        sync
        log "DualSense Bluetooth runtime restored to native LG state."
        return 0
    fi
    return 1
}

prepare_session()
{
    requested_pid=$1
    requested_start=$2
    requested_exe_id=$3
    APP_PID=$requested_pid
    APP_START=$requested_start
    APP_EXE_ID=$requested_exe_id
    valid_decimal "$APP_PID" && valid_decimal "$APP_START" &&
        valid_devino "$APP_EXE_ID" || return 2
    app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID" || return 75
    [ "$(id -u)" = 0 ] || return 77
    [ "$(uname -m)" = aarch64 ] || return 77
    [ "$(uname -r)" = 4.4.84-229.kcl4tv.6 ] || return 77
    [ "$(sha256sum "$BASE_CONFIG" | awk '{print $1}')" = "$BASE_CONFIG_SHA" ] ||
        return 77

    if [ -e "$STATE_FILE" ]; then
        if load_state && app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID"; then
            log "Another DualSense Bluetooth runtime session is active."
            return 75
        fi
        cleanup_session || return 75
        APP_PID=$requested_pid
        APP_START=$requested_start
        APP_EXE_ID=$requested_exe_id
    fi
    baseline_config_is || {
        log "LG gamepad allowlist is not the tested base value; refusing override."
        return 77
    }

    OWNER_NONCE=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
    [ "${#OWNER_NONCE}" = 32 ] || return 75
    BOOT_ID=$(cat /proc/sys/kernel/random/boot_id)
    valid_boot_id "$BOOT_ID" || return 75
    DAEMON_PID=0
    DAEMON_START=0
    DAEMON_EXE_ID=0:0
    PATCH_ADDRESS=0
    write_state PREPARED
    if ! start_watcher; then
        log "Could not arm the DualSense Bluetooth cleanup watcher."
        rm -f "$STATE_FILE"
        return 75
    fi
    write_state ALLOWLIST_INTENT
    app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID" || {
        cleanup_session || true
        return 75
    }
    if ! set_runtime_config "$OURS_SET_ARRAY" "$OWNER_NONCE" ||
       ! runtime_config_is "$OURS_QUERY_ARRAY" "$OWNER_NONCE"; then
        log "Could not verify the volatile DualSense allowlist."
        cleanup_session || true
        return 75
    fi
    write_state ALLOWLIST_ACTIVE
    app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID" || {
        cleanup_session || true
        return 75
    }
    if ! prepare_patch_target; then
        log "LG Bluetooth output-report signature is unsupported."
        cleanup_session || true
        return 77
    fi
    write_state PATCH_INTENT
    app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID" || {
        cleanup_session || true
        return 75
    }
    if ! enable_patch; then
        log "Could not verify the LG Bluetooth output-report correction."
        cleanup_session || true
        return 75
    fi
    app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID" || {
        cleanup_session || true
        return 75
    }
    write_state ACTIVE
    log "DualSense Bluetooth runtime active for app PID $APP_PID."
    return 0
}

start_watcher()
{
    rm -f "$READY_FILE" "$STATE_DIR/watcher.pid"
    launched_watcher=
    CHIAKI_RUNTIME_WATCH_FD=9 \
        /usr/bin/nohup /bin/sh "$STATE_DIR/runtime.sh" watch \
        </dev/null >>"$LOG" 2>&1 &
    launched_watcher=$!
    watcher_pid=
    ready_nonce=
    attempts=0
    while [ "$attempts" -lt 20 ]; do
        if [ -r "$READY_FILE" ]; then
            watcher_pid=$(sed -n '1s/ .*//p' "$READY_FILE" 2>/dev/null || true)
            ready_nonce=$(sed -n '1s/^[^ ]* //p' "$READY_FILE" 2>/dev/null || true)
            [ "$watcher_pid" = "$launched_watcher" ] &&
                [ "$ready_nonce" = "$OWNER_NONCE" ] && break
        fi
        /bin/usleep 100000
        attempts=$((attempts + 1))
    done
    if ! valid_decimal "$watcher_pid" ||
       [ "$watcher_pid" != "$launched_watcher" ] ||
       [ "$ready_nonce" != "$OWNER_NONCE" ] ||
       ! kill -0 "$watcher_pid" 2>/dev/null; then
        kill "$launched_watcher" 2>/dev/null || true
        wait "$launched_watcher" 2>/dev/null || true
        return 1
    fi
    printf '%s\n' "$watcher_pid" > "$STATE_DIR/watcher.pid"
}

watcher_cleanup()
{
    # fd8 was deliberately closed at watcher entry, so this is a distinct open
    # file description. Wait until the activator has either reached ACTIVE or
    # completed its own rollback before inspecting any INTENT phase.
    exec 8>"$ACTIVATION_LOCK_FILE"
    flock -x 8
    [ -e "$STATE_FILE" ] || return 0
    if [ "${FORCE_CLEANUP:-false}" != true ] &&
       load_state && [ "$PHASE" = ACTIVE ] &&
       app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID"; then
        # The activator crossed ACTIVE just before our arm timeout. It still
        # owns a live app lease, so continue monitoring instead of undoing a
        # successful launch behind the app's back.
        return 2
    fi
    cleanup_session
}

watch_session()
{
    load_state || exit 75
    ready_tmp=$(mktemp "$STATE_DIR/.watcher-ready.XXXXXX")
    printf '%s %s\n' "$$" "$OWNER_NONCE" > "$ready_tmp"
    mv -f "$ready_tmp" "$READY_FILE"

    # The watcher is armed before either volatile side effect. Give the root
    # launcher a bounded window to reach ACTIVE; otherwise roll back any INTENT
    # phase even while the app remains alive.
    activation_wait=0
    while app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID"; do
        if ! load_state; then
            exit 0
        fi
        [ "$PHASE" = ACTIVE ] && break
        activation_wait=$((activation_wait + 1))
        if [ "$activation_wait" -ge 15 ]; then
            log "DualSense Bluetooth activation did not reach ACTIVE."
            if watcher_cleanup; then
                exit 0
            else
                cleanup_status=$?
                [ "$cleanup_status" = 2 ] && break
                exit "$cleanup_status"
            fi
        fi
        sleep 1
    done
    health_counter=0
    health_failures=0
    while app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID"; do
        sleep 1
        health_counter=$((health_counter + 1))
        if [ "$health_counter" -ge 5 ]; then
            health_counter=0
            if ! runtime_config_is "$OURS_QUERY_ARRAY" "$OWNER_NONCE" ||
               ! daemon_matches "$DAEMON_PID" "$DAEMON_START" "$DAEMON_EXE_ID" ||
               [ "$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)" != "$BOOT_ID" ] ||
               [ "$(read_signature "$DAEMON_PID" "$PATCH_ADDRESS" 2>/dev/null || true)" != "$SIGNATURE_ON" ]; then
                health_failures=$((health_failures + 1))
                if [ "$health_failures" -ge 2 ]; then
                    log "DualSense Bluetooth runtime drifted; stopping the app fail-closed."
                    if app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID"; then
                        kill -TERM "$APP_PID" 2>/dev/null || true
                    fi
                    stop_wait=0
                    while app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID" &&
                          [ "$stop_wait" -lt 100 ]; do
                        /bin/usleep 100000
                        stop_wait=$((stop_wait + 1))
                    done
                    if app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID"; then
                        log "Chiaki did not quiesce after runtime revocation; killing it."
                        kill -KILL "$APP_PID" 2>/dev/null || true
                        kill_wait=0
                        while app_matches "$APP_PID" "$APP_START" "$APP_EXE_ID" &&
                              [ "$kill_wait" -lt 20 ]; do
                            /bin/usleep 100000
                            kill_wait=$((kill_wait + 1))
                        done
                    fi
                    FORCE_CLEANUP=true
                    break
                fi
            else
                health_failures=0
            fi
        fi
    done
    # The pre-spawned app writer gets socket EOF/PDEATHSIG first and sends a
    # zero-motor report while the correction is still active.
    sleep 2
    watcher_cleanup
}

for parent in /var /var/lib /var/lib/webosbrew; do
    [ -d "$parent" ] && [ ! -L "$parent" ] || exit 77
    [ "$(stat -c '%u:%g:%a' "$parent")" = "0:0:755" ] || exit 77
done
if [ ! -e "$STATE_DIR" ]; then
    mkdir -m 0700 "$STATE_DIR"
fi
[ -d "$STATE_DIR" ] && [ ! -L "$STATE_DIR" ] || exit 77
[ "$(stat -c '%u:%g:%a' "$STATE_DIR")" = "0:0:700" ] || exit 77

if [ "${1:-}" = watch ]; then
    [ "${CHIAKI_RUNTIME_WATCH_FD:-}" = 9 ] || exit 77
    [ -e /proc/self/fd/9 ] || exit 77
    # Do not share the activator's fence OFD. watcher_cleanup opens its own.
    exec 8<&-
    watch_session
    exit $?
fi

exec 9>"$LOCK_FILE"
chmod 0600 "$LOCK_FILE"
flock -x -w 5 9 || exit 75

exec 8>"$ACTIVATION_LOCK_FILE"
chmod 0600 "$ACTIVATION_LOCK_FILE"
flock -x -w 5 8 || exit 75

action=${1:-}
case "$action" in
    prepare)
        [ "$#" = 4 ] || exit 2
        prepare_session "$2" "$3" "$4"
        ;;
    cleanup)
        [ "$#" = 1 ] || exit 2
        if [ -e "$STATE_FILE" ]; then cleanup_session; fi
        ;;
    *)
        echo "usage: $0 prepare APP_PID APP_START APP_EXE_DEVINO | cleanup" >&2
        exit 2
        ;;
esac
