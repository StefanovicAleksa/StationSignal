#!/usr/bin/env bash
# Test suite for deploy/scripts/station-signal-netconfig.sh.
#
# Runs the real script against a temp state dir with stubbed nmcli/systemctl/systemd-run on PATH
# (see stubs/), so every failure mode — including the one that bricked a box's reconfigurability
# for a day — is exercised with zero risk to the machine's actual network. No dependencies beyond
# bash; run it from anywhere:
#
#   bash deploy/scripts/tests/run.sh            # all tests
#   bash deploy/scripts/tests/run.sh stale      # only tests whose name contains "stale"
#
# Every test asserts the script's central invariant somewhere: no failure path may leave the
# pending marker behind.
set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Overridable so a mutated copy can be checked against the suite (i.e. that these tests actually
# fail when the fail-safe behavior is removed).
SCRIPT="${NETCONFIG_SCRIPT:-$TESTS_DIR/../station-signal-netconfig.sh}"
STUBS="$TESTS_DIR/stubs"
FILTER="${1:-}"

PASS=0
FAIL=0
CURRENT=""
WORK=""

# --- harness ----------------------------------------------------------------------------------

setup() {
    WORK="$(mktemp -d)"
    export STATE_DIR="$WORK/state"
    export CONF_FILE="$WORK/netconfig.conf"
    export STUB_LOG="$WORK/stub.log"
    export REQUIRE_ROOT=0
    export PATH="$STUBS:$PATH"
    mkdir -p "$STATE_DIR"
    printf 'CONNECTION_NAME=TestConn\n' >"$CONF_FILE"
    : >"$STUB_LOG"
    unset NMCLI_FAIL_RE NMCLI_CONN_MISSING SYSTEMD_RUN_FAIL SYSTEMD_RUN_SLEEP SYSTEMCTL_TIMER_ACTIVE
    unset STUB_IFACE STUB_ADDRS STUB_GW STUB_METHOD
}

teardown() {
    [ -n "$WORK" ] && rm -rf "$WORK"
    WORK=""
}

# run_script <args...> — invokes the script under test, capturing combined output in $OUT and
# the exit status in $RC (never aborts the suite on a non-zero exit).
run_script() {
    OUT="$(bash "$SCRIPT" "$@" 2>&1)"
    RC=$?
    return 0
}

fail() {
    FAIL=$((FAIL + 1))
    echo "  FAIL: $CURRENT"
    echo "        $1"
    [ -n "${2:-}" ] && echo "        $2"
    return 1
}

assert_rc() {
    [ "$RC" -eq "$1" ] && return 0
    fail "expected exit $1, got $RC" "output: $OUT"
}

assert_rc_nonzero() {
    [ "$RC" -ne 0 ] && return 0
    fail "expected a non-zero exit, got 0" "output: $OUT"
}

assert_no_pending() {
    [ ! -f "$STATE_DIR/pending" ] && [ ! -f "$STATE_DIR/previous.env" ] && return 0
    fail "state files survived — this is the wedge the script must never allow" \
         "state dir: $(ls "$STATE_DIR" 2>/dev/null | tr '\n' ' ')"
}

assert_pending() {
    [ -f "$STATE_DIR/pending" ] && return 0
    fail "expected a pending marker, found none"
}

assert_log_contains() {
    grep -qF -- "$1" "$STUB_LOG" && return 0
    fail "expected an invocation matching: $1" "log: $(tr '\n' '|' <"$STUB_LOG")"
}

assert_log_missing() {
    grep -qF -- "$1" "$STUB_LOG" || return 0
    fail "did not expect an invocation matching: $1" "log: $(tr '\n' '|' <"$STUB_LOG")"
}

assert_file_contains() {
    [ -f "$1" ] && grep -qF -- "$2" "$1" && return 0
    fail "expected $1 to contain: $2" "actual: $(cat "$1" 2>/dev/null)"
}

# apply_ok runs a successful apply, leaving a genuine pending change in place.
apply_ok() {
    run_script apply 192.168.1.77/24 192.168.1.1 90
}

# write_pending <secondsFromNow> forges a pending marker expiring at the given offset.
write_pending() {
    printf 'PENDING=1\nEXPIRES_AT=%s\nNEW_CIDR=192.168.1.77/24\nNEW_GATEWAY=192.168.1.1\n' \
        "$(( $(date +%s) + $1 ))" >"$STATE_DIR/pending"
    printf 'PREV_ADDRESSES=%q\nPREV_GATEWAY=%q\nPREV_METHOD=%q\n' \
        "192.168.1.50/24, 169.254.1.1/24" "192.168.1.1" "manual" >"$STATE_DIR/previous.env"
}

test_case() {
    local name="$1"
    if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then
        return 0
    fi
    CURRENT="$name"
    setup
    if "$2"; then
        PASS=$((PASS + 1))
        echo "  ok: $name"
    fi
    teardown
}

# --- tests ------------------------------------------------------------------------------------

# THE REGRESSION TEST. Reproduces the exact production failure: apply succeeds, then activation
# fails ("No suitable device found ..." → nmcli exit 4). Before the fix this left the profile
# broken and the pending marker on disk forever, blocking every future change. Now activation
# self-heals immediately instead of waiting out the 90s watchdog.
t_activate_failure_self_heals() {
    apply_ok || return 1
    assert_rc 0 || return 1
    assert_pending || return 1

    export NMCLI_FAIL_RE='device reapply|connection up'
    run_script activate
    assert_rc_nonzero || return 1
    assert_no_pending || return 1
    assert_file_contains "$STATE_DIR/activate-result" "RESULT=failed" || return 1
    # It restored the snapshot rather than leaving the new address applied.
    assert_log_contains "nmcli connection modify TestConn ipv4.method manual ipv4.addresses 192.168.1.50/24, 169.254.1.1/24" || return 1
}

# The second half of the original wedge: the auto-revert ran the same failing nmcli and aborted
# under `set -e` before its cleanup line. A revert that cannot reactivate must still clear state.
t_revert_clears_state_even_when_nmcli_fails() {
    apply_ok || return 1
    assert_pending || return 1

    export NMCLI_FAIL_RE='device reapply|connection up'
    run_script revert
    assert_rc_nonzero || return 1
    assert_no_pending || return 1
}

t_revert_succeeds_and_clears_state() {
    apply_ok || return 1
    run_script revert
    assert_rc 0 || return 1
    assert_no_pending || return 1
}

# A marker whose EXPIRES_AT has passed means its auto-revert never ran (or ran and failed). It
# must not block a new apply — this alone would have unwedged the box.
t_stale_pending_does_not_block_apply() {
    write_pending -600
    apply_ok
    assert_rc 0 || return 1
    assert_pending || return 1
    assert_file_contains "$STATE_DIR/pending" "NEW_CIDR=192.168.1.77/24" || return 1
}

# A marker we cannot date is a marker we cannot trust; refusing forever is the worse failure.
t_undateable_pending_does_not_block_apply() {
    : >"$STATE_DIR/pending"
    apply_ok
    assert_rc 0 || return 1
    assert_pending || return 1
}

# A genuinely in-flight change is still refused — with the dedicated exit code the Go side maps
# to a 409 CHANGE_ALREADY_PENDING instead of a generic 500 full of raw shell stderr.
t_live_pending_blocks_apply_with_exit_3() {
    write_pending 300
    apply_ok
    assert_rc 3 || return 1
    assert_pending || return 1
}

# After a reboot the transient systemd timer is gone but the state files survive, so nothing is
# ever scheduled to clear them. reconcile (run at API startup) is what closes that hole.
t_reconcile_clears_orphaned_pending() {
    write_pending 300
    export SYSTEMCTL_TIMER_ACTIVE=0
    run_script reconcile
    assert_rc 0 || return 1
    assert_no_pending || return 1
}

t_reconcile_leaves_live_change_alone() {
    write_pending 300
    export SYSTEMCTL_TIMER_ACTIVE=1
    run_script reconcile
    assert_rc 0 || return 1
    assert_pending || return 1
}

t_reconcile_is_a_noop_when_nothing_pending() {
    run_script reconcile
    assert_rc 0 || return 1
    assert_no_pending || return 1
}

# A failure after the profile was modified but before the watchdog was armed used to leave a
# marker with nothing scheduled to ever clear it. The ERR trap now rolls back.
t_systemd_run_failure_rolls_back() {
    export SYSTEMD_RUN_FAIL=1
    apply_ok
    assert_rc_nonzero || return 1
    assert_no_pending || return 1
    assert_log_contains "nmcli connection modify TestConn ipv4.method manual ipv4.addresses 192.168.1.50/24, 169.254.1.1/24" || return 1
}

# The API used to run this script with the HTTP request's context, so a browser navigating away
# mid-apply killed it outright. The Go side no longer does that, but the script must survive it
# regardless — the box is about to lose its network, aborted requests are the normal case.
t_sigterm_midflight_rolls_back() {
    export SYSTEMD_RUN_SLEEP=2
    bash "$SCRIPT" apply 192.168.1.77/24 192.168.1.1 90 >"$WORK/out" 2>&1 &
    local pid=$!
    # Wait for the marker to appear, i.e. we're past the modify and inside the systemd-run sleep.
    local waited=0
    while [ ! -f "$STATE_DIR/pending" ] && [ "$waited" -lt 50 ]; do
        sleep 0.1
        waited=$((waited + 1))
    done
    kill -TERM "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    RC=$?
    OUT="$(cat "$WORK/out")"
    assert_rc_nonzero || return 1
    assert_no_pending || return 1
}

# systemd timers default to AccuracySec=1min, which turned "activate in 1s" into "activate in 1
# to 61 seconds" — the box sat on its old address for tens of seconds while the browser polled
# the new one and correctly saw nothing. Both timers must pin their accuracy.
t_timers_pin_their_accuracy() {
    apply_ok || return 1
    assert_rc 0 || return 1
    assert_log_contains "--unit=station-signal-netconfig-activate --on-active=1s --timer-property=AccuracySec=100ms" || return 1
    assert_log_contains "--unit=station-signal-netconfig-revert --on-active=90s --timer-property=AccuracySec=100ms" || return 1
}

# The wrong-device bug itself: activation must name the device, never let NetworkManager choose.
t_activation_pins_the_device() {
    apply_ok || return 1
    run_script activate
    assert_rc 0 || return 1
    assert_log_contains "nmcli device reapply wlan0" || return 1
    assert_log_missing "nmcli connection up TestConn" || return 1
}

t_activation_falls_back_to_pinned_up_when_reapply_refuses() {
    apply_ok || return 1
    export NMCLI_FAIL_RE='device reapply'
    run_script activate
    assert_rc 0 || return 1
    assert_log_contains "nmcli connection up TestConn ifname wlan0" || return 1
    assert_file_contains "$STATE_DIR/activate-result" "RESULT=ok" || return 1
}

t_confirm_clears_state() {
    apply_ok || return 1
    run_script confirm
    assert_rc 0 || return 1
    assert_no_pending || return 1
}

t_confirm_without_pending_fails() {
    run_script confirm
    assert_rc 1 || return 1
}

# Confirming touches no NetworkManager state, so a broken conf file must never be able to block
# making a good change permanent.
t_confirm_works_with_broken_conf() {
    apply_ok || return 1
    rm -f "$CONF_FILE"
    run_script confirm
    assert_rc 0 || return 1
    assert_no_pending || return 1
}

# load_conf exits early here; the EXIT trap has to have been armed before it for state to clear.
t_revert_clears_state_when_conf_is_broken() {
    apply_ok || return 1
    rm -f "$CONF_FILE"
    run_script revert
    assert_rc_nonzero || return 1
    assert_no_pending || return 1
}

t_status_reports_pending_and_activation_result() {
    apply_ok || return 1
    run_script activate
    run_script status
    assert_rc 0 || return 1
    case "$OUT" in
        *PENDING=1*NEW_CIDR=192.168.1.77/24*RESULT=ok*) ;;
        *) fail "status output missing pending/result details" "output: $OUT"; return 1 ;;
    esac
}

t_status_reports_nothing_pending() {
    run_script status
    assert_rc 0 || return 1
    case "$OUT" in
        *PENDING=0*) ;;
        *) fail "expected PENDING=0" "output: $OUT"; return 1 ;;
    esac
}

t_invalid_input_is_rejected_without_touching_anything() {
    run_script apply 169.254.5.5/24 - 90
    assert_rc 1 || return 1
    assert_no_pending || return 1
    assert_log_missing "connection modify" || return 1
}

# --- main -------------------------------------------------------------------------------------

echo "station-signal-netconfig.sh"

test_case "activate failure self-heals (the production wedge)" t_activate_failure_self_heals
test_case "revert clears state even when nmcli fails"          t_revert_clears_state_even_when_nmcli_fails
test_case "revert succeeds and clears state"                   t_revert_succeeds_and_clears_state
test_case "stale pending does not block apply"                 t_stale_pending_does_not_block_apply
test_case "undateable pending does not block apply"            t_undateable_pending_does_not_block_apply
test_case "live pending blocks apply with exit 3"              t_live_pending_blocks_apply_with_exit_3
test_case "reconcile clears orphaned pending"                  t_reconcile_clears_orphaned_pending
test_case "reconcile leaves a live change alone"               t_reconcile_leaves_live_change_alone
test_case "reconcile is a no-op when nothing pending"          t_reconcile_is_a_noop_when_nothing_pending
test_case "systemd-run failure rolls back"                     t_systemd_run_failure_rolls_back
test_case "SIGTERM mid-apply rolls back"                       t_sigterm_midflight_rolls_back
test_case "timers pin their accuracy"                          t_timers_pin_their_accuracy
test_case "activation pins the device"                         t_activation_pins_the_device
test_case "activation falls back to pinned up"                 t_activation_falls_back_to_pinned_up_when_reapply_refuses
test_case "confirm clears state"                               t_confirm_clears_state
test_case "confirm without pending fails"                      t_confirm_without_pending_fails
test_case "confirm works with a broken conf"                   t_confirm_works_with_broken_conf
test_case "revert clears state when conf is broken"            t_revert_clears_state_when_conf_is_broken
test_case "status reports pending and activation result"       t_status_reports_pending_and_activation_result
test_case "status reports nothing pending"                     t_status_reports_nothing_pending
test_case "invalid input touches nothing"                      t_invalid_input_is_rejected_without_touching_anything

echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
