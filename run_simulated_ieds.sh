#!/usr/bin/env bash
# Runs a few distinguishable fake IEDs continuously, so the frontend's Network Scan page has
# something real to discover and connect to during manual testing. Uses ied_sim_app, built from
# station_signal_daemon/integration_tests/simulation_app_tests/ (a standalone copy of the daemon's
# own ied_simulator test fixture, adapted to run several concurrent, network-distinguishable
# instances instead of one hardcoded loopback-only one — see that directory's own header
# comments for the full rationale).
#
# Subnet discovery (IedDiscovery_scanSubnet) enumerates every host in the scanned interface's own
# configured IPv4 subnet and is capped at 1024 candidate hosts — loopback's real /8 blows past
# that instantly, which is why this script gives the simulated devices their own small, dedicated
# subnet on a Linux "dummy" interface instead of reusing "lo".
#
# Privilege model mirrors station_signal_daemon/run_all_tests.sh: this script re-execs itself under
# sudo if not already root, since both the network setup (ip link/addr) and each simulated
# device's own real GOOSE publishing (raw AF_PACKET socket, CAP_NET_RAW) need it.
#
# Usage: ./run_simulated_ieds.sh [count]
#   count - how many simulated IEDs to run (default 3).
# Stop everything with Ctrl+C — this also tears down the dummy interface.
#
# In addition to the $count devices above, one more device ("SimAuth", on a fixed reserved
# address) always starts with ACSE password auth required (SimServer_requireAuthentication via
# ied_sim_app's optional trailing [password] arg) — connecting to it without a password should
# surface the API's AUTH_REQUIRED (401) response / the frontend's password prompt; the password
# is printed below. Every Sim1..SimN device is unprotected, same as before this was added.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$ROOT/station_signal_daemon/integration_tests/simulation_app_tests"
SIM_BIN="$SIM_DIR/ied_sim_app"
SIM_FILESTORE="$SIM_DIR/served_files/"

COUNT="${1:-3}"
IFACE="ied-sim0"
SUBNET_PREFIX="10.250.99"
IFACE_CIDR="${SUBNET_PREFIX}.1/28"
MMS_PORT="${IED_SIM_MMS_PORT:-102}"
MAX_COUNT=12 # /28 usable hosts (.2-.14) minus the interface's own .1 and .14 (reserved for SimAuth below)
AUTH_IP="${SUBNET_PREFIX}.14" # fixed/reserved, so it doesn't shift as COUNT changes between runs
AUTH_NAME="SimAuth"
AUTH_PASSWORD="secret123"

if [ "$COUNT" -lt 1 ] || [ "$COUNT" -gt "$MAX_COUNT" ]; then
    echo "error: count must be between 1 and $MAX_COUNT (got $COUNT)" >&2
    exit 1
fi

command -v gcc >/dev/null 2>&1 || { echo "error: 'gcc' is required but not found on PATH" >&2; exit 1; }
command -v ip >/dev/null 2>&1 || { echo "error: 'ip' is required but not found on PATH" >&2; exit 1; }

echo "==> Building ied_sim_app"
make -C "$SIM_DIR"

echo "==> Setting up dummy interface $IFACE ($IFACE_CIDR)"
if ! ip link show "$IFACE" >/dev/null 2>&1; then
    ip link add name "$IFACE" type dummy
fi
# The base /28 address must be added FIRST: the daemon's subnet discovery
# (getifaddrs, ied_discovery_netif.c) takes the first AF_INET entry it finds for this
# interface name and stops, so this one - the one whose netmask actually describes the
# subnet - has to be the one already present before any of the per-device /32s below.
if ! ip -4 addr show dev "$IFACE" | grep -q "$IFACE_CIDR"; then
    ip addr add "$IFACE_CIDR" dev "$IFACE"
fi
ip link set "$IFACE" up

# Unlike loopback (where the kernel treats the whole 127.0.0.0/8 as always-bindable),
# a dummy interface only accepts bind() to addresses actually assigned to it - binding to
# an address that's merely "in range" of the /28 above fails with EADDRNOTAVAIL, and
# libiec61850's TcpServerSocket_create (hal/socket/linux/socket_linux.c) swallows that
# failure with no log line at all, so each simulated device's own address must be
# explicitly added here too (as a /32 - no need to widen the routed subnet further).
for i in $(seq 1 "$COUNT"); do
    ip_addr="${SUBNET_PREFIX}.$((i + 1))"
    if ! ip -4 addr show dev "$IFACE" | grep -q "${ip_addr}/32"; then
        ip addr add "${ip_addr}/32" dev "$IFACE"
    fi
done
if ! ip -4 addr show dev "$IFACE" | grep -q "${AUTH_IP}/32"; then
    ip addr add "${AUTH_IP}/32" dev "$IFACE"
fi

PIDS=()

cleanup() {
    echo ""
    echo "==> Shutting down simulated IEDs"
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "==> Tearing down $IFACE"
    ip link delete "$IFACE" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

echo "==> Starting $COUNT simulated IED(s) on ${SUBNET_PREFIX}.x:${MMS_PORT}"
for i in $(seq 1 "$COUNT"); do
    ip_addr="${SUBNET_PREFIX}.$((i + 1))"
    ied_name="Sim${i}"
    log_file="/tmp/ied_sim_app_${i}.log"
    "$SIM_BIN" "$ip_addr" "$MMS_PORT" "$ied_name" "$IFACE" "$SIM_FILESTORE" >"$log_file" 2>&1 &
    PIDS+=("$!")
    echo "    $ied_name  ${ip_addr}:${MMS_PORT}  (log: $log_file)"
done

auth_log_file="/tmp/ied_sim_app_auth.log"
"$SIM_BIN" "$AUTH_IP" "$MMS_PORT" "$AUTH_NAME" "$IFACE" "$SIM_FILESTORE" "$AUTH_PASSWORD" >"$auth_log_file" 2>&1 &
PIDS+=("$!")
echo "    $AUTH_NAME  ${AUTH_IP}:${MMS_PORT}  (log: $auth_log_file)  [requires password: $AUTH_PASSWORD]"

sleep 1
for pid in "${PIDS[@]}"; do
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "error: a simulated IED process exited immediately — check the logs above" >&2
        exit 1
    fi
done

echo ""
echo "In the frontend's Network Scan page, use:"
echo "    Interface: $IFACE"
echo "    MMS Port:  $MMS_PORT"
echo ""
echo "To test the password-prompt flow: connect to $AUTH_NAME ($AUTH_IP) — the first attempt"
echo "should show the frontend's password prompt; enter '$AUTH_PASSWORD' to complete the connect."
echo ""
echo "(run_dev.sh must be running separately for the daemon/API/frontend to be reachable.)"
echo "Press Ctrl+C to stop everything."
echo ""

wait
