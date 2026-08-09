#!/usr/bin/env bash
# Runs every test in this repo: unit tests (tests/), every E2E suite under
# integration_tests/ (ied_model, mms_report_client, scl_bootstrap,
# ipc_dispatcher, ied_discovery, ied_model_online_loader, goose_subscriber,
# orchestration, orchestration_local_file, scan_dispatcher,
# scan_orchestration, device_manager, control_dispatcher), and the one
# genuine pass/fail probe under tools/smoke_tests/
# (goose_loopback_smoke_test.c - proves a bare GoosePublisher/GooseReceiver
# pair round-trips a real GOOSE frame over lo). Every OTHER file under
# tools/smoke_tests/ is a manual diagnostic tool, not an automated test
# (requires a live external IED, a hardcoded machine-specific path, or just
# prints output for a human to read rather than asserting pass/fail) -
# deliberately not run here; see each one's own header comment.
#
# Requires sudo (goose_subscriber/orchestration/orchestration_local_file/
# device_manager/control_dispatcher's real-device case, plus the smoke test
# itself, all open a raw AF_PACKET socket for GOOSE - CAP_NET_RAW, per
# CLAUDE.md) - script re-execs itself under sudo if needed, so every suite
# runs under one consistent umbrella even though most of them don't
# individually need it.
#
# Always `make clean`s each Makefile-based suite before `make run`: these
# Makefiles link third_party/lib/libiec61850.a as a linker flag, not a
# tracked prerequisite, so a swapped-in .a (e.g. after rebuilding the
# vendored library) would otherwise go unnoticed by make's timestamp check
# and silently link a stale binary. The smoke test (no Makefile, built via
# the same raw gcc command as CLAUDE.md's own Commands section) is always
# rebuilt from scratch into a fresh temp path for the same reason.
#
# Tracks pass/fail per suite AND aggregates every individual Unity
# "N Tests M Failures P Ignored" summary line across every Makefile-based
# suite (tests/ alone runs many separate Unity binaries, each emitting its
# own such line) into one grand total across the whole run, printed at the
# end alongside the per-suite pass/fail list. The smoke test isn't
# Unity-based (a single PASSED/FAILED probe, not "N Tests") - it contributes
# exactly 1 to the total test count and 0 or 1 to the total failure count.
#
# IMPORTANT (see CLAUDE.md's own note on this): this suite list is
# hand-maintained, not auto-discovered - update the run_suite/run_smoke_test
# calls below whenever a test suite (a new tests/<feature>/ Makefile entry,
# a new integration_tests/<feature>/ directory, or a new genuine pass/fail
# probe under tools/smoke_tests/) is added or removed.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAILED=()
TOTAL_TESTS=0
TOTAL_FAILURES=0
TOTAL_IGNORED=0

run_suite() {
    local name="$1" dir="$2"
    echo "==> ${name}"

    local logfile
    logfile="$(mktemp)"

    (cd "${ROOT}/${dir}" && make clean) >/dev/null 2>&1 || true

    if ! (cd "${ROOT}/${dir}" && make run) 2>&1 | tee "${logfile}"; then
        FAILED+=("${name}")
    fi

    local counts
    counts="$(grep -oE '^[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored' "${logfile}" \
        | awk '{ t += $1; f += $3; i += $5 } END { print t+0, f+0, i+0 }')" || true
    local suite_tests suite_failures suite_ignored
    read -r suite_tests suite_failures suite_ignored <<< "${counts:-0 0 0}"

    TOTAL_TESTS=$((TOTAL_TESTS + suite_tests))
    TOTAL_FAILURES=$((TOTAL_FAILURES + suite_failures))
    TOTAL_IGNORED=$((TOTAL_IGNORED + suite_ignored))

    rm -f "${logfile}"
}

# For the one non-Makefile, non-Unity probe under tools/smoke_tests/ - builds
# fresh via the same raw gcc command as CLAUDE.md's own Commands section,
# runs it, and folds its single PASSED/FAILED outcome into the same
# FAILED[]/TOTAL_* accounting run_suite uses, so the final summary covers it
# too without needing a second reporting path.
run_smoke_test() {
    local name="$1" src="$2"
    shift 2
    echo "==> ${name}"

    local bin
    bin="$(mktemp -u)"

    if ! gcc -g -Wall -I"${ROOT}/src" -idirafter "${ROOT}/third_party/include" \
            "${ROOT}/${src}" -o "${bin}" -L"${ROOT}/third_party/lib" "$@"; then
        echo "${name}: build failed"
        FAILED+=("${name}")
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
        return
    fi

    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if ! "${bin}"; then
        FAILED+=("${name}")
        TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
    fi

    rm -f "${bin}"
}

run_suite "unit tests" "tests"
run_suite "e2e: ied_model" "integration_tests/ied_model"
run_suite "e2e: mms_report_client" "integration_tests/mms_report_client"
run_suite "e2e: scl_bootstrap" "integration_tests/scl_bootstrap"
run_suite "e2e: ipc_dispatcher" "integration_tests/ipc_dispatcher"
run_suite "e2e: ied_discovery" "integration_tests/ied_discovery"
run_suite "e2e: ied_model_online_loader" "integration_tests/ied_model_online_loader"
run_suite "e2e: goose_subscriber" "integration_tests/goose_subscriber"
run_suite "e2e: orchestration" "integration_tests/orchestration"
run_suite "e2e: orchestration_local_file" "integration_tests/orchestration_local_file"
run_suite "e2e: scan_dispatcher" "integration_tests/scan_dispatcher"
run_suite "e2e: scan_orchestration" "integration_tests/scan_orchestration"
run_suite "e2e: device_manager" "integration_tests/device_manager"
run_suite "e2e: control_dispatcher" "integration_tests/control_dispatcher"
run_smoke_test "smoke: goose_loopback" "tools/smoke_tests/goose_loopback_smoke_test.c" \
        -liec61850 -lhal -lpthread

echo
echo "==> Totals across all suites: ${TOTAL_TESTS} Tests, ${TOTAL_FAILURES} Failures, ${TOTAL_IGNORED} Ignored"
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "All test suites passed."
else
    echo "FAILED suites: ${FAILED[*]}"
    exit 1
fi
