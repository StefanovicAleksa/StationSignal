#!/usr/bin/env bash
# Test suite for deploy/scripts/station-signal-clearlogs.sh.
#
# Separate from run.sh (which tests station-signal-netconfig.sh's state machine and needs stubbed
# nmcli/systemctl) because this script has no state, no arguments and no external commands — but it
# does run as root, so its three behaviours are worth pinning down anyway.
#
# The script hardcodes /var/log/station_signal by design, so each test runs a copy with that one
# line rewritten to a temp dir. That is also the only way to test it without touching the real
# machine's logs.
#
#   bash deploy/scripts/tests/run_clearlogs.sh
set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${CLEARLOGS_SCRIPT:-$TESTS_DIR/../station-signal-clearlogs.sh}"

PASS=0
FAIL=0

# Each test below runs in a subshell, so a failure counter incremented in there would never reach
# this scope. fail() therefore records into a per-test flag file whose path the subshell inherits,
# and run_test reads it back afterwards — without this the suite reported "0 failed" while printing
# FAIL lines, which is worse than having no suite at all.
fail() {
    echo "  FAIL: $1"
    echo 1 > "$TEST_FAILED_FLAG"
}

# run_test <name> <function>
run_test() {
    local name="$1" fn="$2"
    echo "== $name"
    TEST_FAILED_FLAG="$(mktemp)"
    export TEST_FAILED_FLAG
    ( "$fn" )
    if [ -s "$TEST_FAILED_FLAG" ]; then
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi
    rm -f "$TEST_FAILED_FLAG"
}

# Stages a copy of the script pointed at $1 and echoes its path.
stage() {
    local logdir="$1" copy
    copy="$(mktemp)"
    cp "$SCRIPT" "$copy"
    sed -i "s|^LOG_DIR=.*|LOG_DIR=\"$logdir\"|" "$copy"
    echo "$copy"
}

test_truncates_matching_only() {
    dir="$(mktemp -d)"; mkdir -p "$dir/logs"
    printf 'old daemon noise\n' > "$dir/logs/station-signal-daemon.log"
    printf 'old api noise\n'    > "$dir/logs/station-signal-api.log"
    printf 'keep me\n'          > "$dir/logs/notes.txt"
    script="$(stage "$dir/logs")"

    out="$(bash "$script")"
    [ "$out" = "CLEARED=2" ] || fail "expected CLEARED=2, got '$out'"
    [ ! -s "$dir/logs/station-signal-daemon.log" ] || fail "daemon log not emptied"
    [ ! -s "$dir/logs/station-signal-api.log" ]    || fail "api log not emptied"
    [ -s "$dir/logs/notes.txt" ]                   || fail "unrelated file was touched"
    rm -rf "$dir" "$script"
}

# The whole reason this truncates instead of deleting: the daemon holds each log open in append
# mode for its entire process lifetime and never reopens (src/log.h). If the file were unlinked,
# that handle would keep writing into an orphaned inode and the logs would silently stop.
test_truncates_in_place() {
    dir="$(mktemp -d)"; mkdir -p "$dir/logs"
    log="$dir/logs/station-signal-daemon.log"
    printf 'previous session\n' > "$log"
    inode_before="$(stat -c %i "$log")"
    script="$(stage "$dir/logs")"

    # A live append-mode writer, exactly as log.h holds one.
    exec 9>>"$log"
    bash "$script" >/dev/null
    echo "after clear" >&9
    exec 9>&-

    inode_after="$(stat -c %i "$log")"
    [ "$inode_before" = "$inode_after" ] || fail "file was replaced (inode changed) rather than truncated"
    [ "$(cat "$log")" = "after clear" ] || fail "expected only post-clear content, got: $(cat "$log")"
    rm -rf "$dir" "$script"
}

test_missing_dir() {
    dir="$(mktemp -d)"
    script="$(stage "$dir/never-created")"

    out="$(bash "$script")"
    rc=$?
    [ "$rc" -eq 0 ]           || fail "expected exit 0, got $rc"
    [ "$out" = "CLEARED=0" ]  || fail "expected CLEARED=0, got '$out'"
    rm -rf "$dir" "$script"
}

test_empty_dir() {
    dir="$(mktemp -d)"; mkdir -p "$dir/logs"
    script="$(stage "$dir/logs")"

    out="$(bash "$script")"
    rc=$?
    [ "$rc" -eq 0 ]          || fail "expected exit 0, got $rc"
    [ "$out" = "CLEARED=0" ] || fail "expected CLEARED=0 when the glob matches nothing, got '$out'"
    rm -rf "$dir" "$script"
}

run_test "truncates every station-signal-*.log and leaves anything else alone" test_truncates_matching_only
run_test "truncates in place, so a writer holding the file open keeps working" test_truncates_in_place
run_test "a missing log directory is not an error" test_missing_dir
run_test "an empty log directory is not an error" test_empty_dir

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
