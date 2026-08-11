#!/usr/bin/env bash
# Builds the daemon and runs all three pieces of the app together for local development:
# station_signal_daemon (rebuilt from source), station_signal_api (supervising that daemon), and
# the station_signal_frontend dev server.
#
# Privilege model mirrors the sudo-elevation pattern already used by
# station_signal_daemon/run_all_tests.sh and station_signal_api/run_integration_tests.sh: real GOOSE
# reception needs CAP_NET_RAW, and this repo's convention is to run the whole
# daemon-spawning process under sudo rather than setcap the binary. So only the API (and the
# daemon it spawns as a child) runs under sudo here — building the daemon (gcc) and running the
# frontend dev server (vite) need no elevation and run as the invoking user.
#
# Usage: ./run_dev.sh
# Stop everything with Ctrl+C.
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    echo "error: run this WITHOUT sudo (./run_dev.sh, not sudo ./run_dev.sh)." >&2
    echo "       It elevates only the API sub-process internally. Running the whole" >&2
    echo "       script as root puts 'npm run dev' under sudo's sanitized PATH too," >&2
    echo "       which resolves to a different (often too-old) Node than your normal" >&2
    echo "       shell's, breaking the frontend dev server with an obscure syntax error." >&2
    exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAEMON_DIR="$ROOT/station_signal_daemon"
API_DIR="$ROOT/station_signal_api"
FRONTEND_DIR="$ROOT/station_signal_frontend"


# Per-user paths (not a fixed shared /tmp path): the daemon/API used to always run under
# sudo, which left these owned by root — a later non-sudo run couldn't overwrite them at
# the same path (/tmp has the sticky bit, so a normal user can't even rm a root-owned file
# there). Namespacing by uid means a stale root-owned binary from an old invocation is
# simply never in the way, and two different users on the same box don't collide either.
DAEMON_BIN="${DAEMON_BIN:-/tmp/station_signal_daemon-$(id -u)}"
API_BIN="${API_BIN:-/tmp/station_signal_api-$(id -u)}"
API_HTTP_ADDR="${STATION_SIGNAL_API_HTTP_ADDR:-:8080}"
API_PORT="${API_HTTP_ADDR##*:}"

for tool in gcc go npm curl sudo; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: '$tool' is required but not found on PATH" >&2; exit 1; }
done

echo "==> Checking sudo access (needed to run the API, which spawns the daemon)"
sudo -v

echo "==> Building daemon"
"$DAEMON_DIR/rebuild_proj.sh" "$DAEMON_BIN"

echo "==> Building API"
(cd "$API_DIR" && go build -o "$API_BIN" ./cmd/station_signal_api)

if [ ! -d "$FRONTEND_DIR/node_modules" ]; then
    echo "==> Installing frontend dependencies"
    (cd "$FRONTEND_DIR" && npm install)
fi

API_PID=""
FRONTEND_PID=""

cleanup() {
    echo ""
    echo "==> Shutting down"
    if [ -n "$FRONTEND_PID" ]; then
        kill "$FRONTEND_PID" 2>/dev/null || true
    fi
    if [ -n "$API_PID" ]; then
        # main.go handles SIGTERM itself: it stops the daemon child (SIGINT, then SIGKILL
        # after a grace period) before exiting, so this alone tears down both processes.
        sudo kill -TERM "$API_PID" 2>/dev/null || true
        sleep 2
        # Fallback in case a signal got lost somewhere across the sudo boundary.
        sudo pkill -f -- "$API_BIN" 2>/dev/null || true
        sudo pkill -f -- "$DAEMON_BIN" 2>/dev/null || true
    fi
    wait 2>/dev/null || true
}
trap cleanup INT TERM EXIT

echo "==> Starting API on ${API_HTTP_ADDR} (sudo required — the daemon it supervises needs CAP_NET_RAW for GOOSE)"
# -mode dev as a flag, not an env var: sudo scrubs the environment, so STATION_SIGNAL_MODE set out
# here would never reach the process. The API turns this into debug logging for itself, the HTTP
# access log, and STATION_SIGNAL_LOG_LEVEL=debug for the daemon it spawns.
sudo "$API_BIN" -daemon-bin "$DAEMON_BIN" -http-addr "$API_HTTP_ADDR" -mode dev &
API_PID=$!

echo "==> Waiting for the API to become ready"
api_ready=""
for _ in $(seq 1 30); do
    if curl -fsS "http://127.0.0.1:${API_PORT}/api/health" >/dev/null 2>&1; then
        api_ready=1
        break
    fi
    if ! sudo kill -0 "$API_PID" 2>/dev/null; then
        echo "error: the API process (sudo $API_BIN) exited before becoming ready" >&2
        exit 1
    fi
    sleep 1
done
if [ -z "$api_ready" ]; then
    echo "error: API did not respond on http://127.0.0.1:${API_PORT}/api/health after 30s" >&2
    exit 1
fi

echo "==> Starting frontend dev server"
(cd "$FRONTEND_DIR" && VITE_API_BASE_URL="http://localhost:${API_PORT}" npm run dev) &
FRONTEND_PID=$!

echo ""
echo "API:      http://127.0.0.1:${API_PORT}"
echo "Frontend: see the URL vite prints above (default http://localhost:5173)"
echo "Press Ctrl+C to stop everything."
echo ""

wait -n "$API_PID" "$FRONTEND_PID"
