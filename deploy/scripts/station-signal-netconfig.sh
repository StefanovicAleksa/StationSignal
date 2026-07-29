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
#   activate                                    bring the just-applied profile into effect
#                                                (what the detached activate unit runs); reverts
#                                                immediately if activation fails
#   confirm                                     cancel the pending auto-revert, make it permanent
#   revert                                      restore the pre-apply configuration (also what
#                                                the scheduled auto-revert itself runs)
#   reconcile                                   clear an orphaned pending change left behind by a
#                                                reboot/crash (run at API startup)
#   status                                      print whether a change is pending, and how the
#                                                last activation went
#
# Does its own strict input validation below as defense in depth, even though sudoers already
# restricts *what* can invoke this script at all — see internal/features/network/domain.Config's
# own (redundant, and that's fine) validation on the Go side.
#
# Install as /opt/station_signal/bin/station-signal-netconfig.sh, root-owned, mode 0700 (see
# deploy/setup.sh). Reads the NetworkManager connection to manage from /etc/station-signal/
# netconfig.conf (CONNECTION_NAME=...), written by setup.sh at install time.
#
# THE INVARIANT THIS SCRIPT EXISTS TO UPHOLD: no failure path may leave $PENDING_FILE behind.
# That marker blocks every future apply, and it is not self-expiring on disk — a leftover marker
# is how this feature bricked its own reconfigurability once already (a failed `nmcli connection
# up` inside the auto-revert aborted the function under `set -e` before its cleanup line, and the
# box then refused every subsequent IP change until the file was removed by hand). Every command
# below that can touch the marker either clears it via a trap or clears it before returning.
#
# -E (errtrace) matters specifically: without it the ERR trap cmd_apply arms would not fire for
# failures inside a shell function, which is exactly where every failure here happens.
set -Eeuo pipefail

# Paths and unit names are env-overridable purely so deploy/scripts/tests/ can exercise this
# against a temp state dir with stubbed nmcli/systemctl. Production always takes the defaults:
# sudo's env_reset (the Debian/Ubuntu default) strips these from the caller's environment, so
# the unprivileged station-signal user cannot redirect any of it through the sudoers rule.
: "${CONF_FILE:=/etc/station-signal/netconfig.conf}"
: "${STATE_DIR:=/opt/station_signal/netconfig-state}"
: "${REVERT_UNIT:=station-signal-netconfig-revert}"
: "${ACTIVATE_UNIT:=station-signal-netconfig-activate}"
# How long past a pending change's own EXPIRES_AT before it's considered abandoned rather than
# in-flight. The OS-level auto-revert fires exactly at EXPIRES_AT, so anything meaningfully past
# it means that revert never ran (or ran and failed) — see cmd_reconcile.
: "${STALE_GRACE_SECONDS:=15}"
: "${REQUIRE_ROOT:=1}"
# systemd timer units default to AccuracySec=1min: the timer fires anywhere in the minute AFTER
# its nominal deadline, because systemd coalesces wakeups to save power. That default silently
# turned `--on-active=1s` into "activate the new address in 1 to 61 seconds" — measured at 6s,
# 17s and 25s on this hardware. The technician's browser meanwhile polls the new address and
# correctly reports "nothing answered", because for those tens of seconds the box really is still
# on its old address. It also smeared the auto-revert deadline from 90s to 90-150s, and let a
# confirm land in the same second as the revert it was racing. Neither timer here is a
# power-management concern; both want to fire when they say they will.
: "${TIMER_ACCURACY:=100ms}"

PREV_FILE="$STATE_DIR/previous.env"
PENDING_FILE="$STATE_DIR/pending"
RESULT_FILE="$STATE_DIR/activate-result"
SELF="$(readlink -f "$0")"

# Exit codes the Go side distinguishes (internal/features/network/data/runner.go):
EXIT_PENDING=3   # refused: a genuine, not-yet-expired change is already pending

# The fixed, permanent recovery address every box carries — see deploy/README.md and
# internal/features/network/domain.RecoveryAddress. Never removed by this script; always
# re-asserted alongside whatever primary address is being applied. Kept in sync by hand with
# domain.RecoveryAddress/recoveryBlock — if that ever changes, update it here too.
RECOVERY_CIDR="169.254.1.1/24"
RECOVERY_BLOCK_RE='^169\.254\.'

usage() {
    echo "usage: $0 {apply <cidr> <gateway|-> <timeoutSeconds>|activate|confirm|revert|reconcile|status}" >&2
    exit 64
}

require_root() {
    if [ "$REQUIRE_ROOT" -eq 0 ]; then
        return 0
    fi
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
    DEVICE="$(resolve_device)"
}

# resolve_device pins the network device this profile belongs to. Without this, `nmcli connection
# up <name>` lets NetworkManager pick a device itself — and it can pick the wrong one and then
# fail outright ("No suitable device found for this connection (device enp34s0 not available
# because profile is not compatible with device (mismatching interface name))" was observed on a
# Wi-Fi profile while an unrelated ethernet NIC was mid-DHCP). The profile's own
# connection.interface-name is the authoritative answer; GENERAL.DEVICES is the fallback for
# profiles pinned by MAC rather than by name.
resolve_device() {
    local dev
    dev="$(nmcli -g connection.interface-name connection show "$CONNECTION_NAME" 2>/dev/null || true)"
    if [ -z "$dev" ]; then
        dev="$(nmcli -g GENERAL.DEVICES connection show "$CONNECTION_NAME" 2>/dev/null || true)"
        dev="${dev%%,*}"
    fi
    printf '%s' "$dev"
}

# activate_profile puts the profile's current settings into effect on its own device.
#
# `nmcli device reapply` is tried first because it applies changed ipv4.* settings to a device
# in place, without deactivating it — no carrier drop, no Wi-Fi re-association, and no chance of
# NetworkManager choosing a different device. `connection up ... ifname` is the fallback for the
# cases reapply refuses (device currently disconnected, or a property change that genuinely needs
# full reactivation); it still pins the device explicitly.
activate_profile() {
    if [ -n "${DEVICE:-}" ] && nmcli device reapply "$DEVICE"; then
        return 0
    fi
    if [ -n "${DEVICE:-}" ]; then
        nmcli connection up "$CONNECTION_NAME" ifname "$DEVICE"
    else
        nmcli connection up "$CONNECTION_NAME"
    fi
}

# clear_state tears down the pending change's bookkeeping: both transient units and both state
# files. Every step is best-effort — this runs from EXIT traps on failure paths, and must never
# itself be the reason the marker survives.
clear_state() {
    systemctl stop "${REVERT_UNIT}.timer" >/dev/null 2>&1 || true
    systemctl stop "${ACTIVATE_UNIT}.timer" >/dev/null 2>&1 || true
    systemctl reset-failed "$REVERT_UNIT" "${REVERT_UNIT}.timer" \
        "$ACTIVATE_UNIT" "${ACTIVATE_UNIT}.timer" >/dev/null 2>&1 || true
    rm -f "$PENDING_FILE" "$PREV_FILE"
}

# restore_previous rewrites the profile from the pre-apply snapshot and reactivates it. Returns
# non-zero if any part fails — callers treat that as "report it, but keep cleaning up".
restore_previous() {
    [ -f "$PREV_FILE" ] || return 0
    # shellcheck disable=SC1090
    source "$PREV_FILE"
    if [ -z "${PREV_GATEWAY:-}" ]; then
        nmcli connection modify "$CONNECTION_NAME" ipv4.method "${PREV_METHOD:-manual}" ipv4.addresses "$PREV_ADDRESSES" ipv4.gateway "" || return 1
    else
        nmcli connection modify "$CONNECTION_NAME" ipv4.method "${PREV_METHOD:-manual}" ipv4.addresses "$PREV_ADDRESSES" ipv4.gateway "$PREV_GATEWAY" || return 1
    fi
    activate_profile || return 1
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

# guard_pending refuses a new apply while one is genuinely in flight — but treats a marker whose
# own EXPIRES_AT has passed as abandoned rather than authoritative. The marker is not
# self-expiring on disk and survives reboots (transient systemd timers do not), so without this
# an auto-revert that never ran, or ran and failed, would block every future change forever.
guard_pending() {
    [ -f "$PENDING_FILE" ] || return 0

    local expires_at now
    expires_at="$(awk -F= '/^EXPIRES_AT=/{print $2}' "$PENDING_FILE" 2>/dev/null || true)"
    [[ "$expires_at" =~ ^[0-9]+$ ]] || expires_at=0
    now="$(date +%s)"

    if [ "$expires_at" -gt 0 ] && [ "$now" -le $((expires_at + STALE_GRACE_SECONDS)) ]; then
        echo "error: a network change is already pending — confirm or revert it first" >&2
        exit "$EXIT_PENDING"
    fi

    # Undateable markers (missing/garbage EXPIRES_AT) count as stale too: a marker we can't date
    # is a marker we can't trust, and refusing forever is the worse failure.
    echo "note: clearing an abandoned pending change (expired $((now - expires_at))s ago)" >&2
    restore_previous || echo "warning: could not fully restore the abandoned change's previous config" >&2
    clear_state
}

apply_rollback() {
    local rc=$?
    trap - ERR INT TERM EXIT
    # On a signal path $? is whatever the last completed command returned — bash defers the trap
    # until the running foreground command finishes, so it is routinely 0 here. Rolling back and
    # then exiting 0 would tell the API the apply succeeded; never report success from here.
    [ "$rc" -eq 0 ] && rc=1
    echo "error: apply failed (exit $rc) — rolling back to the previous configuration" >&2
    restore_previous || echo "warning: rollback could not fully restore the previous config" >&2
    clear_state
    exit "$rc"
}

cmd_apply() {
    [ $# -eq 3 ] || usage
    local cidr="$1" gateway="$2" timeout="$3"
    validate_cidr "$cidr"
    validate_gateway "$gateway"
    validate_timeout "$timeout"
    load_conf

    mkdir -p "$STATE_DIR"
    guard_pending
    rm -f "$RESULT_FILE"

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

    # From here on the profile is (about to be) modified, so any failure — including this process
    # being killed, which happens if the API's caller aborts the HTTP request mid-apply — must
    # undo it rather than leave both a half-applied profile and a marker blocking the retry.
    trap apply_rollback ERR INT TERM

    local new_addrs="$cidr,$RECOVERY_CIDR"
    if [ "$gateway" = "-" ]; then
        nmcli connection modify "$CONNECTION_NAME" ipv4.method manual ipv4.addresses "$new_addrs" ipv4.gateway ""
    else
        nmcli connection modify "$CONNECTION_NAME" ipv4.method manual ipv4.addresses "$new_addrs" ipv4.gateway "$gateway"
    fi

    # NEW_CIDR/NEW_GATEWAY are here so `status` can describe the pending change to the API even
    # after the API process restarted and lost its own in-memory copy of it.
    {
        echo "PENDING=1"
        echo "EXPIRES_AT=$(($(date +%s) + timeout))"
        printf 'NEW_CIDR=%s\n' "$cidr"
        printf 'NEW_GATEWAY=%s\n' "$gateway"
    } >"$PENDING_FILE"

    # Schedule the real, OS-level, API-process-independent auto-revert. --on-active is relative
    # to scheduling time, so this fires exactly `timeout` seconds from now regardless of whether
    # the API process (or this script) is still around by then. --collect so a failed run garbage
    # collects itself instead of lingering and colliding with the next apply's --unit=.
    systemd-run --collect --unit="$REVERT_UNIT" --on-active="${timeout}s" \
        --timer-property=AccuracySec="$TIMER_ACCURACY" \
        --description="station-signal auto-revert of a provisional network change" \
        "$SELF" revert

    # Activation is the actually-disruptive step — it can drop the interface for anywhere from
    # under a second to (observed) 40+ seconds if DHCP/carrier negotiation stalls. This script
    # runs synchronously inside the API's HTTP handler; if activation happened here, it could
    # tear down the very TCP connection carrying this request/response before the response ever
    # reaches the technician's browser (the "respond before cutting the cord" requirement). So:
    # return to the caller — and let the HTTP response go out — first, and only activate in a
    # detached unit afterward.
    #
    # It runs `$SELF activate` rather than nmcli directly so that a failed activation reverts
    # itself within seconds instead of leaving the box dark until the auto-revert above fires,
    # and so the outcome is recorded in $RESULT_FILE for the API to report. The auto-revert
    # remains the backstop; nothing here depends on this unit succeeding.
    #
    # --on-active=1s rather than --no-block: `nmcli connection modify` above returns as soon as
    # NetworkManager has persisted the new settings over D-Bus, but that doesn't guarantee NM's
    # internal device-compatibility cache has finished reconciling yet. A short fixed delay is
    # cheaper and more robust than trying to detect "settled".
    systemd-run --collect --unit="$ACTIVATE_UNIT" --on-active=1s \
        --timer-property=AccuracySec="$TIMER_ACCURACY" \
        --description="station-signal: activate a provisionally-applied network change" \
        "$SELF" activate

    trap - ERR INT TERM
}

cmd_activate() {
    load_conf
    mkdir -p "$STATE_DIR"

    if activate_profile; then
        printf 'RESULT=ok\nAT=%s\n' "$(date +%s)" >"$RESULT_FILE"
        return 0
    fi

    printf 'RESULT=failed\nAT=%s\n' "$(date +%s)" >"$RESULT_FILE"
    echo "error: activating '$CONNECTION_NAME' failed — reverting now rather than waiting for the auto-revert" >&2
    cmd_revert || true
    return 1
}

# cmd_confirm deliberately does not load_conf: confirming touches no NetworkManager state, and a
# broken/missing conf file must never be able to block making a good change permanent.
cmd_confirm() {
    [ -f "$PENDING_FILE" ] || { echo "error: no pending network change to confirm" >&2; exit 1; }
    clear_state
}

# cmd_revert restores the pre-apply configuration. The EXIT trap is armed FIRST, before anything
# that can fail (load_conf exits on a missing conf file or renamed connection; nmcli can fail to
# reactivate), because clearing the pending marker is not conditional on the restore succeeding.
# A revert that couldn't reactivate the interface still reports failure — but it reports it with
# the marker already gone, so the next attempt isn't blocked by this one.
cmd_revert() {
    mkdir -p "$STATE_DIR"
    trap clear_state EXIT

    load_conf
    if [ ! -f "$PREV_FILE" ]; then
        echo "nothing to revert"
        return 0
    fi

    if ! restore_previous; then
        echo "error: could not fully restore the previous configuration — pending state cleared anyway;" >&2
        echo "       the box remains reachable at the fixed recovery address (see deploy/README.md)" >&2
        return 1
    fi
}

# cmd_reconcile cleans up after a reboot or crash. Transient systemd units do not survive a
# reboot but the state files do, so a box that went down mid-change comes back up with a pending
# marker and nothing scheduled to ever clear it. Run at API startup.
cmd_reconcile() {
    if [ ! -f "$PENDING_FILE" ]; then
        echo "PENDING=0"
        return 0
    fi
    if systemctl is-active --quiet "${REVERT_UNIT}.timer" 2>/dev/null; then
        echo "pending change still has its auto-revert armed — leaving it alone"
        return 0
    fi
    echo "orphaned pending change with no auto-revert armed — reverting it"
    cmd_revert
}

cmd_status() {
    if [ -f "$PENDING_FILE" ]; then
        cat "$PENDING_FILE"
    else
        echo "PENDING=0"
    fi
    [ -f "$RESULT_FILE" ] && cat "$RESULT_FILE" || true
}

require_root
[ $# -ge 1 ] || usage
case "$1" in
    apply) shift; cmd_apply "$@" ;;
    activate) cmd_activate ;;
    confirm) cmd_confirm ;;
    revert) cmd_revert ;;
    reconcile) cmd_reconcile ;;
    status) cmd_status ;;
    *) usage ;;
esac
