#!/usr/bin/env bash
# Runs the integration test suite under integration_tests/ (scanning, reporting, crashrearm)
# against the REAL ied_reporter_daemon binary and, for reporting, the daemon's own manual IED
# simulator — nothing mocked, mirroring ied_reporter_daemon/run_all_tests.sh's own approach.
#
# Requires sudo: reporting's START_REPORTING success path needs the daemon to open a raw
# GOOSE socket (CAP_NET_RAW), same reason the daemon's own suite re-execs itself under sudo.
# scanning and crashrearm don't individually need it, but run under the same umbrella so the
# whole suite is exercised consistently in one invocation.
#
# GOTOOLCHAIN=local is exported so `go test`/`go build` never attempt a network toolchain
# download regardless of the invoking shell's environment.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo --preserve-env=GOTOOLCHAIN "$0" "$@"
fi

export GOTOOLCHAIN=local
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT}"

echo "==> go test -tags=integration ./integration_tests/..."
go test -tags=integration -v -timeout 10m ./integration_tests/...
