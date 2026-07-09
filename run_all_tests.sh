#!/usr/bin/env bash
# Runs all tests in this repo: unit tests (tests/) then every E2E test under
# integration_tests/ (ied_model, mms_report_client, scl_bootstrap,
# ipc_dispatcher, ied_discovery, goose_subscriber, orchestration).
# Requires sudo (goose_subscriber/orchestration open a raw AF_PACKET socket
# for GOOSE - CAP_NET_RAW, per CLAUDE.md) - script re-execs itself under
# sudo if needed.
# Always `make clean`s each suite before `make run`: these Makefiles link
# third_party/lib/libiec61850.a as a linker flag, not a tracked prerequisite,
# so a swapped-in .a (e.g. after rebuilding the vendored library) would
# otherwise go unnoticed by make's timestamp check and silently link a stale
# binary.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAILED=()

run_suite() {
    local name="$1" dir="$2"
    echo "==> ${name}"
    if ! (cd "${ROOT}/${dir}" && make clean >/dev/null 2>&1; cd "${ROOT}/${dir}" && make run); then
        FAILED+=("${name}")
    fi
}

run_suite "unit tests" "tests"
run_suite "e2e: ied_model" "integration_tests/ied_model"
run_suite "e2e: mms_report_client" "integration_tests/mms_report_client"
run_suite "e2e: scl_bootstrap" "integration_tests/scl_bootstrap"
run_suite "e2e: ipc_dispatcher" "integration_tests/ipc_dispatcher"
run_suite "e2e: ied_discovery" "integration_tests/ied_discovery"
run_suite "e2e: goose_subscriber" "integration_tests/goose_subscriber"
run_suite "e2e: orchestration" "integration_tests/orchestration"

echo
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "All test suites passed."
else
    echo "FAILED suites: ${FAILED[*]}"
    exit 1
fi