#!/usr/bin/env bash
# Runs every test in this repo: unit tests (tests/) then every E2E suite
# under integration_tests/ (ied_model, mms_report_client, scl_bootstrap,
# ipc_dispatcher, ied_discovery, ied_model_online_loader, goose_subscriber,
# orchestration, orchestration_local_file, scan_dispatcher,
# scan_orchestration, device_manager, control_dispatcher).
#
# Requires sudo (goose_subscriber/orchestration/orchestration_local_file/
# device_manager/control_dispatcher's real-device case all open a raw
# AF_PACKET socket for GOOSE - CAP_NET_RAW, per CLAUDE.md) - script re-execs
# itself under sudo if needed, so every suite runs under one consistent
# umbrella even though most of them don't individually need it.
#
# Always `make clean`s each suite before `make run`: these Makefiles link
# third_party/lib/libiec61850.a as a linker flag, not a tracked prerequisite,
# so a swapped-in .a (e.g. after rebuilding the vendored library) would
# otherwise go unnoticed by make's timestamp check and silently link a stale
# binary.
#
# Tracks pass/fail per suite AND aggregates every individual Unity
# "N Tests M Failures P Ignored" summary line across every suite (tests/
# alone runs many separate Unity binaries, each emitting its own such line)
# into one grand total across the whole run, printed at the end alongside
# the per-suite pass/fail list.
#
# IMPORTANT (see CLAUDE.md's own note on this): this suite list is
# hand-maintained, not auto-discovered - update the run_suite calls below
# whenever a test suite (a new tests/<feature>/ Makefile entry or a new
# integration_tests/<feature>/ directory) is added or removed.
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

echo
echo "==> Totals across all suites: ${TOTAL_TESTS} Tests, ${TOTAL_FAILURES} Failures, ${TOTAL_IGNORED} Ignored"
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "All test suites passed."
else
    echo "FAILED suites: ${FAILED[*]}"
    exit 1
fi
