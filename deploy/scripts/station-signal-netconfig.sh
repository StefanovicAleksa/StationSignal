#!/usr/bin/env bash
# Privileged helper for the Settings page's remote static-IP reconfiguration feature — the one
# place this box's unprivileged station-signal user (running station_signal_api) can reach root,
# via the narrowly-scoped sudoers rule in deploy/sudoers/station-signal-netconfig. Mirrors the
# daemon binary's own setcap grant in spirit: one script, fixed vocabulary, root-owned so the
# unprivileged caller can't tamper with its contents.
#
# Fixed command vocabulary only — no free-form shell, no interface name taken over the network:
#   apply <cidr> <gateway|-> <timeoutSeconds>   provisionally apply a new primary address,
#                                                scheduling an OS-level auto-revert after
#                                                timeoutSeconds unless confirmed first
#   confirm                                     cancel the pending auto-revert, make it permanent
#   revert                                      restore the pre-apply configuration (also what
#                                                the scheduled auto-revert itself runs)
#   status                                      print whether a change is pending (debugging)
#
# Does its own strict input validation below as defense in depth, even though sudoers already
# restricts *what* can invoke this script at all — see internal/features/network/domain.Config's
# own (redundant, and that's fine) validation on the Go side.
#
# Install as /opt/station_signal/bin/station-signal-netconfig.sh, root-owned, mode 0700 (see
# deploy/setup.sh). Reads the NetworkManager connection to manage from /etc/station-signal/
# netconfig.conf (CONNECTION_NAME=...), written by setup.sh at install time.
set -euo pipefail

CONF_FILE="/etc/station-signal/netconfig.conf"
STATE_DIR="/opt/station_signal/netconfig-state"
PREV_FILE="$STATE_DIR/previous.env"
PENDING_FILE="$STATE_DIR/pending"
REVERT_UNIT="station-signal-netconfig-revert"

# The fixed, permanent recovery address every box carries — see deploy/README.md and
# internal/features/network/domain.RecoveryAddress. Never removed by this script; always
# re-asserted alongside whatever primary address is being applied. Kept in sync by hand with
# domain.RecoveryAddress/recoveryBlock — if that ever changes, update it here too.
RECOVERY_CIDR="169.254.1.1/24"
RECOVERY_BLOCK_RE='^169\.254\.'

usage() {
    echo "usage: $0 {apply <cidr> <gateway|-> <timeoutSeconds>|confirm|revert|status}" >&2
    exit 64
}

require_root() {
    [ "$(id -u)" -eq 0 ] || { echo "error: must run as root (invoke via sudo)" >&2; exit 1; }
}

load_conf() {
    [ -f "$CONF_FILE" ] || { echo "error: $CONF_FILE not found — run deploy/setup.sh first" >&2; exit 1; }
    # shellcheck disable=SC1090
    source "$CONF_FILE"
    : "${CONNECTION_NAME:?CONNECTION_NAME not set in $CONF_FILE}"
    nmcli connection show "$CONNECTION_NAME" >/dev/null 2>&1 || {
        echo "error: NetworkManager connection '$CONNECTION_NAME' not found" >&2
        exit 1
    }
}

validate_octets() {
    local ip="$1" a b c d
    IFS='.' read -r a b c d <<<"$ip"
    for octet in "$a" "$b" "$c" "$d"; do
        [[ "$octet" =~ ^[0-9]+$ ]] && [ "$octet" -ge 0 ] && [ "$octet" -le 255 ] || return 1
    done
}

validate_cidr() {
    local cidr="$1" ip prefix
    [[ "$cidr" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}/([0-9]|[1-2][0-9]|3[0-2])$ ]] || {
        echo "error: invalid CIDR: $cidr" >&2; exit 1;
    }
    ip="${cidr%/*}"
    prefix="${cidr#*/}"
    validate_octets "$ip" || { echo "error: invalid CIDR: $cidr" >&2; exit 1; }
    [ "$prefix" -ge 1 ] && [ "$prefix" -le 30 ] || {
        echo "error: prefix length must be between /1 and /30: $cidr" >&2; exit 1;
    }
    if [[ "$ip" =~ $RECOVERY_BLOCK_RE ]]; then
        echo "error: $cidr is in the 169.254.0.0/16 block reserved for the fixed recovery address" >&2
        exit 1
    fi
}

validate_gateway() {
    local gw="$1"
    [ "$gw" = "-" ] && return 0
    [[ "$gw" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] && validate_octets "$gw" || {
        echo "error: invalid gateway: $gw" >&2; exit 1;
    }
}

validate_timeout() {
    local t="$1"
    [[ "$t" =~ ^[0-9]+$ ]] && [ "$t" -gt 0 ] || { echo "error: invalid timeout: $t" >&2; exit 1; }
}

cmd_apply() {
    [ $# -eq 3 ] || usage
    local cidr="$1" gateway="$2" timeout="$3"
    validate_cidr "$cidr"
    validate_gateway "$gateway"
    validate_timeout "$timeout"
    load_conf

    if [ -f "$PENDING_FILE" ]; then
        echo "error: a network change is already pending — confirm or revert it first" >&2
        exit 1
    fi

    mkdir -p "$STATE_DIR"

    # Snapshot the current config as "last known good" before touching anything, so revert has
    # something real to restore.
    local prev_addrs prev_gw prev_method
    prev_addrs="$(nmcli -g ipv4.addresses connection show "$CONNECTION_NAME")"
    prev_gw="$(nmcli -g ipv4.gateway connection show "$CONNECTION_NAME")"
    prev_method="$(nmcli -g ipv4.method connection show "$CONNECTION_NAME")"
    # %q-quote each value: ipv4.addresses comes back comma-*and-space* separated once the
    # recovery address is present (nmcli -g's multi-value format), and an unquoted value
    # containing a space breaks `source` on read — bash parses everything after the first
    # space as a separate command, which crashes cmd_revert before it ever gets to nmcli.
    {
        printf 'PREV_ADDRESSES=%q\n' "$prev_addrs"
        printf 'PREV_GATEWAY=%q\n' "$prev_gw"
        printf 'PREV_METHOD=%q\n' "$prev_method"
    } >"$PREV_FILE"

    local new_addrs="$cidr,$RECOVERY_CIDR"
    if [ "$gateway" = "-" ]; then
        nmcli connection modify "$CONNECTION_NAME" ipv4.method manual ipv4.addresses "$new_addrs" ipv4.gateway ""
    else
        nmcli connection modify "$CONNECTION_NAME" ipv4.method manual ipv4.addresses "$new_addrs" ipv4.gateway "$gateway"
    fi

    {
        echo "PENDING=1"
        echo "EXPIRES_AT=$(($(date +%s) + timeout))"
    } >"$PENDING_FILE"

    # Schedule the real, OS-level, API-process-independent auto-revert. --on-active is relative
    # to scheduling time, so this fires exactly `timeout` seconds from now regardless of whether
    # the API process (or this script) is still around by then.
    systemd-run --unit="$REVERT_UNIT" --on-active="${timeout}s" \
        --description="station-signal auto-revert of a provisional network change" \
        "$(readlink -f "$0")" revert

    # `nmcli connection up` is the actually-disruptive step — it can drop the interface for
    # anywhere from under a second to (observed) 40+ seconds if DHCP/carrier negotiation stalls.
    # This script runs synchronously inside the API's HTTP handler; if the reactivation happened
    # here, it could tear down the very TCP connection carrying this request/response before the
    # response ever reaches the technician's browser (the "respond before cutting the cord"
    # requirement). So: return to the caller — and let the HTTP response go out — first, and only
    # bring the connection up in a detached unit afterward. If it fails, the change never becomes
    # reachable, the frontend's polling never confirms it, and the auto-revert scheduled above
    # restores the old config on its own — the safety net does not depend on this succeeding.
    #
    # --on-active=1s rather than --no-block: `nmcli connection modify` above returns as soon as
    # NetworkManager has persisted the new settings over D-Bus, but that doesn't guarantee NM's
    # internal device-compatibility cache (which connections are "available" for which devices,
    # recomputed off the ipv4.* change) has finished reconciling yet. Firing `connection up`
    # in the same instant (--no-block) raced that reconciliation and intermittently failed with
    # "device not available ... mismatching interface name" even though the profile's
    # connection.interface-name correctly names this device — a stale-cache artifact, not a real
    # mismatch. A short, fixed delay is cheaper and more robust than trying to detect "settled".
    systemd-run --unit="station-signal-netconfig-activate" --on-active=1s \
        --description="station-signal: activate a provisionally-applied network change" \
        nmcli connection up "$CONNECTION_NAME"
}

cmd_confirm() {
    load_conf
    [ -f "$PENDING_FILE" ] || { echo "error: no pending network change to confirm" >&2; exit 1; }
    systemctl stop "${REVERT_UNIT}.timer" >/dev/null 2>&1 || true
    systemctl reset-failed "$REVERT_UNIT" >/dev/null 2>&1 || true
    rm -f "$PENDING_FILE" "$PREV_FILE"
}

cmd_revert() {
    load_conf
    if [ ! -f "$PREV_FILE" ]; then
        echo "nothing to revert"
        exit 0
    fi
    # shellcheck disable=SC1090
    source "$PREV_FILE"

    if [ -z "${PREV_GATEWAY:-}" ]; then
        nmcli connection modify "$CONNECTION_NAME" ipv4.method "${PREV_METHOD:-manual}" ipv4.addresses "$PREV_ADDRESSES" ipv4.gateway ""
    else
        nmcli connection modify "$CONNECTION_NAME" ipv4.method "${PREV_METHOD:-manual}" ipv4.addresses "$PREV_ADDRESSES" ipv4.gateway "$PREV_GATEWAY"
    fi
    nmcli connection up "$CONNECTION_NAME"

    systemctl stop "${REVERT_UNIT}.timer" >/dev/null 2>&1 || true
    systemctl reset-failed "$REVERT_UNIT" >/dev/null 2>&1 || true
    rm -f "$PENDING_FILE" "$PREV_FILE"
}

cmd_status() {
    if [ -f "$PENDING_FILE" ]; then
        cat "$PENDING_FILE"
    else
        echo "PENDING=0"
    fi
}

require_root
[ $# -ge 1 ] || usage
case "$1" in
    apply) shift; cmd_apply "$@" ;;
    confirm) cmd_confirm ;;
    revert) cmd_revert ;;
    status) cmd_status ;;
    *) usage ;;
esac
