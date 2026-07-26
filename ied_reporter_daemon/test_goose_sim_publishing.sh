#!/usr/bin/env bash
set -euo pipefail

# Isolates one question: does ied_simulator's IedServer-integrated GOOSE
# publisher (integration_tests/ied_simulator/src/sim_server.c) ever put a
# single GOOSE frame on the wire at all, independent of our own
# goose_subscriber code? Builds a standalone probe
# (tools/smoke_tests/goose_sim_publish_only.c) that starts the real
# simulator and does nothing else, captures "lo" with tcpdump while it runs,
# and reports whether any GOOSE (ethertype 0x88b8) frames were seen.
#
# Needs CAP_NET_RAW for both tcpdump and the simulator's raw GOOSE socket -
# run the whole script as root:
#   sudo ./test_goose_sim_publishing.sh

if [ "$EUID" -ne 0 ]; then
    echo "This script needs raw-socket access for both tcpdump and the simulator." >&2
    echo "Run it with: sudo ./test_goose_sim_publishing.sh" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

BIN=/tmp/goose_sim_publish_only
CAPTURE=/tmp/goose_sim_publish_capture.pcap
CAPTURE_DURATION=16

echo "==> Building $BIN"
gcc -g -Wall -Isrc -Iintegration_tests/ied_simulator/src -idirafter third_party/include \
    tools/smoke_tests/goose_sim_publish_only.c \
    integration_tests/ied_simulator/src/sim_server.c \
    -o "$BIN" \
    -Lthird_party/lib -liec61850 -lhal -lmxml -lpthread

rm -f "$CAPTURE"

echo "==> Starting tcpdump capture on 'lo' for ${CAPTURE_DURATION}s (ether proto 0x88b8)"
timeout "$CAPTURE_DURATION" tcpdump -i lo -n 'ether proto 0x88b8' -w "$CAPTURE" &
TCPDUMP_PID=$!

# Give tcpdump a moment to actually bind before the simulator starts publishing.
sleep 1

echo "==> Running the publish-only simulator probe"
"$BIN"

echo "==> Waiting for tcpdump to finish"
wait "$TCPDUMP_PID" 2>/dev/null || true

echo ""
echo "==================== RESULT ===================="
if [ -s "$CAPTURE" ]; then
    COUNT=$(tcpdump -n -r "$CAPTURE" 2>/dev/null | wc -l)
    if [ "$COUNT" -gt 0 ]; then
        echo "Captured $COUNT GOOSE frame(s) on 'lo'. The simulator IS publishing. Detail:"
        tcpdump -n -r "$CAPTURE" -v
    else
        echo "Capture file exists but contains 0 packets. The simulator published NOTHING."
    fi
else
    echo "No capture file produced (tcpdump likely failed to start) - inconclusive."
fi
echo "=================================================="
