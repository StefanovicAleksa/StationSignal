#!/usr/bin/env bash
# One-shot installer: builds the daemon, API, and frontend, then installs everything needed to
# reach this app from any PC on the substation LAN at http://stationsignal.local — avahi (mDNS),
# nginx (reverse proxy + static frontend host), and a systemd service that keeps the API (and the
# daemon it supervises) running across crashes/reboots. See deploy/README.md for the manual,
# step-by-step version of everything this script automates.
#
# mDNS, not a DNS server: the substation's actual client fleet is technician laptops with
# hand-configured static IPs (commonly left over from direct IED work via tools like IEDScout),
# so a DHCP-based auto-config approach can't reach them — they never send a DHCP request in the
# first place. avahi/mDNS doesn't care how a client got its address, so it works with zero
# client-side setup either way.
#
# Usage: ./deploy/setup.sh   (run WITHOUT sudo — see below)
# Safe to re-run: every install step overwrites in place rather than failing on "already exists."
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    echo "error: run this WITHOUT sudo (./deploy/setup.sh, not sudo ./deploy/setup.sh)." >&2
    echo "       It elevates only the specific privileged steps (apt, systemctl, useradd," >&2
    echo "       setcap, copying into /etc and /opt) internally via sudo. Running the whole" >&2
    echo "       script as root gives 'go'/'npm' sudo's sanitized PATH instead of yours," >&2
    echo "       which can resolve to a different (often too-old) toolchain — same pitfall" >&2
    echo "       documented in run_dev.sh." >&2
    exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAEMON_DIR="$ROOT/station_signal_daemon"
API_DIR="$ROOT/station_signal_api"
FRONTEND_DIR="$ROOT/station_signal_frontend"
DEPLOY_DIR="$ROOT/deploy"

INSTALL_ROOT="/opt/station_signal"
SERVICE_USER="station-signal"
HOSTNAME_NAME="stationsignal.local"
AVAHI_HOST_NAME="stationsignal"

echo "==> Preflight: checking required tools"
for tool in gcc go npm curl sudo ip sed; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: '$tool' is required but not found on PATH" >&2; exit 1; }
done

echo "==> Detecting this box's LAN IP (read-only — no network config is changed)"
BOX_IP="${BOX_IP:-$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if ($i=="src") print $(i+1)}')}"
if [ -z "$BOX_IP" ]; then
    echo "error: could not auto-detect this box's LAN IP. Re-run with BOX_IP=<ip> ./deploy/setup.sh" >&2
    exit 1
fi
echo "    box IP: $BOX_IP"

echo "==> Building daemon"
DAEMON_BUILD="$(mktemp -d)/station_signal_daemon"
"$DAEMON_DIR/rebuild_proj.sh" "$DAEMON_BUILD"

echo "==> Building API"
API_BUILD="$(mktemp -d)/station_signal_api"
(cd "$API_DIR" && go build -o "$API_BUILD" ./cmd/station_signal_api)

echo "==> Building frontend"
if [ ! -d "$FRONTEND_DIR/node_modules" ]; then
    (cd "$FRONTEND_DIR" && npm install)
fi
(cd "$FRONTEND_DIR" && npm run build)

echo "==> Installing artifacts to $INSTALL_ROOT"
sudo mkdir -p "$INSTALL_ROOT/bin" "$INSTALL_ROOT/frontend-dist" "$INSTALL_ROOT/structure_files"
sudo cp "$DAEMON_BUILD" "$INSTALL_ROOT/bin/station_signal_daemon"
sudo cp "$API_BUILD" "$INSTALL_ROOT/bin/station_signal_api"
sudo rm -rf "${INSTALL_ROOT:?}/frontend-dist"/*
sudo cp -r "$FRONTEND_DIR/dist/." "$INSTALL_ROOT/frontend-dist/"

echo "==> Granting the daemon raw-socket capability (setcap, no root service needed)"
sudo setcap cap_net_raw+ep "$INSTALL_ROOT/bin/station_signal_daemon"

echo "==> Ensuring service user '$SERVICE_USER' exists"
id -u "$SERVICE_USER" >/dev/null 2>&1 || sudo useradd --system --no-create-home "$SERVICE_USER"
sudo chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_ROOT/structure_files"

echo "==> Installing avahi (mDNS) so $HOSTNAME_NAME resolves"
command -v avahi-daemon >/dev/null 2>&1 || sudo apt-get install -y avahi-daemon
if grep -q '^host-name=' /etc/avahi/avahi-daemon.conf; then
    sudo sed -i "s/^host-name=.*/host-name=$AVAHI_HOST_NAME/" /etc/avahi/avahi-daemon.conf
elif grep -q '^#host-name=' /etc/avahi/avahi-daemon.conf; then
    sudo sed -i "s/^#host-name=.*/host-name=$AVAHI_HOST_NAME/" /etc/avahi/avahi-daemon.conf
else
    sudo sed -i "/^\[server\]/a host-name=$AVAHI_HOST_NAME" /etc/avahi/avahi-daemon.conf
fi
sudo systemctl enable --now avahi-daemon >/dev/null
sudo systemctl restart avahi-daemon

echo "==> Installing nginx config"
command -v nginx >/dev/null 2>&1 || sudo apt-get install -y nginx
sudo cp "$DEPLOY_DIR/nginx/stationsignal.conf" /etc/nginx/sites-available/stationsignal.conf
sudo ln -sf /etc/nginx/sites-available/stationsignal.conf /etc/nginx/sites-enabled/stationsignal.conf
# Our site is default_server (see the comment in deploy/nginx/stationsignal.conf) so that a
# client can reach the app by bare IP even where mDNS doesn't resolve (e.g. some Android
# browsers). The stock default site must go, or nginx will refuse to start (duplicate
# default_server).
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t
sudo systemctl reload nginx

echo "==> Installing systemd service"
sudo cp "$DEPLOY_DIR/systemd/station-signal-api.service" /etc/systemd/system/station-signal-api.service
sudo systemctl daemon-reload
sudo systemctl enable --now station-signal-api

echo ""
echo "==> Done."
echo ""
echo "This box's LAN IP: $BOX_IP"
echo "$HOSTNAME_NAME is live via avahi/mDNS — works for statically-addressed clients with zero"
echo "per-laptop setup (Windows 10/11, macOS, and Linux all resolve it natively). Known gap: some"
echo "Android browsers don't resolve .local names reliably — those can still reach the app via"
echo "http://$BOX_IP directly (nginx's default_server)."
echo ""
echo "Verify:"
echo "  On this box:   curl http://127.0.0.1:8080/health"
echo "                 curl -H \"Host: $HOSTNAME_NAME\" http://127.0.0.1/"
echo "  From a LAN PC: open http://$HOSTNAME_NAME in a browser"
