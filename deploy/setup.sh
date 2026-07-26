#!/usr/bin/env bash
# One-shot installer: builds the daemon, API, and frontend, then installs everything needed to
# reach this app from any PC on the substation LAN at http://stationsignal.com — dnsmasq (local
# DNS), nginx (reverse proxy + static frontend host), and a systemd service that keeps the API
# (and the daemon it supervises) running across crashes/reboots. See deploy/README.md for the
# manual, step-by-step version of everything this script automates, and for what it deliberately
# leaves out (static IP / router DHCP reservation, router DNS setting — too router-specific and
# risky to automate blind).
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
DAEMON_DIR="$ROOT/ied_reporter_daemon"
API_DIR="$ROOT/ied_reporter_api"
FRONTEND_DIR="$ROOT/ied_reporter_frontend"
DEPLOY_DIR="$ROOT/deploy"

INSTALL_ROOT="/opt/ied_reporter"
SERVICE_USER="ied-reporter"
HOSTNAME_NAME="stationsignal.com"

echo "==> Preflight: checking required tools"
for tool in gcc go npm curl sudo ip ss sed; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: '$tool' is required but not found on PATH" >&2; exit 1; }
done

echo "==> Detecting network facts (read-only — no network config is changed)"
BOX_IP="${BOX_IP:-$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if ($i=="src") print $(i+1)}')}"
if [ -z "$BOX_IP" ]; then
    echo "error: could not auto-detect this box's LAN IP. Re-run with BOX_IP=<ip> ./deploy/setup.sh" >&2
    exit 1
fi
UPSTREAM_DNS="${UPSTREAM_DNS:-$(awk '/^nameserver/ && $2 != "127.0.0.1" && $2 != "127.0.0.53" {print $2; exit}' /etc/resolv.conf)}"
if [ -z "$UPSTREAM_DNS" ]; then
    echo "error: could not auto-detect an upstream DNS server from /etc/resolv.conf." >&2
    echo "       Re-run with UPSTREAM_DNS=<ip> ./deploy/setup.sh (usually the router's IP)." >&2
    exit 1
fi
echo "    box IP:       $BOX_IP"
echo "    upstream DNS: $UPSTREAM_DNS"

echo "==> Checking port 53 is free for dnsmasq"
if ss -tlnp 2>/dev/null | grep -q ':53 '; then
    echo "error: something is already listening on port 53 (systemd-resolved and" >&2
    echo "       NetworkManager's built-in dnsmasq are common culprits on Raspberry Pi OS" >&2
    echo "       Bookworm). Resolve that first — see the dnsmasq section of deploy/README.md" >&2
    echo "       — rather than let this script silently break the box's existing DNS." >&2
    exit 1
fi

echo "==> Building daemon"
DAEMON_BUILD="$(mktemp -d)/ied_reporter_daemon"
"$DAEMON_DIR/rebuild_proj.sh" "$DAEMON_BUILD"

echo "==> Building API"
API_BUILD="$(mktemp -d)/ied_reporter_api"
(cd "$API_DIR" && go build -o "$API_BUILD" ./cmd/ied_reporter_api)

echo "==> Building frontend"
if [ ! -d "$FRONTEND_DIR/node_modules" ]; then
    (cd "$FRONTEND_DIR" && npm install)
fi
(cd "$FRONTEND_DIR" && npm run build)

echo "==> Installing artifacts to $INSTALL_ROOT"
sudo mkdir -p "$INSTALL_ROOT/bin" "$INSTALL_ROOT/frontend-dist" "$INSTALL_ROOT/structure_files"
sudo cp "$DAEMON_BUILD" "$INSTALL_ROOT/bin/ied_reporter_daemon"
sudo cp "$API_BUILD" "$INSTALL_ROOT/bin/ied_reporter_api"
sudo rm -rf "${INSTALL_ROOT:?}/frontend-dist"/*
sudo cp -r "$FRONTEND_DIR/dist/." "$INSTALL_ROOT/frontend-dist/"

echo "==> Granting the daemon raw-socket capability (setcap, no root service needed)"
sudo setcap cap_net_raw+ep "$INSTALL_ROOT/bin/ied_reporter_daemon"

echo "==> Ensuring service user '$SERVICE_USER' exists"
id -u "$SERVICE_USER" >/dev/null 2>&1 || sudo useradd --system --no-create-home "$SERVICE_USER"
sudo chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_ROOT/structure_files"

echo "==> Installing dnsmasq config for $HOSTNAME_NAME"
command -v dnsmasq >/dev/null 2>&1 || sudo apt-get install -y dnsmasq
sed -e "s/<BOX_STATIC_IP>/$BOX_IP/" -e "s/<UPSTREAM_DNS_IP>/$UPSTREAM_DNS/" \
    "$DEPLOY_DIR/dnsmasq/stationsignal.conf" | sudo tee /etc/dnsmasq.d/stationsignal.conf >/dev/null
sudo systemctl restart dnsmasq

echo "==> Installing nginx config"
command -v nginx >/dev/null 2>&1 || sudo apt-get install -y nginx
sudo cp "$DEPLOY_DIR/nginx/stationsignal.conf" /etc/nginx/sites-available/stationsignal.conf
sudo ln -sf /etc/nginx/sites-available/stationsignal.conf /etc/nginx/sites-enabled/stationsignal.conf
sudo nginx -t
sudo systemctl reload nginx

echo "==> Installing systemd service"
sudo cp "$DEPLOY_DIR/systemd/ied-reporter-api.service" /etc/systemd/system/ied-reporter-api.service
sudo systemctl daemon-reload
sudo systemctl enable --now ied-reporter-api

echo ""
echo "==> Done."
echo ""
echo "This box's LAN IP: $BOX_IP"
echo "For $HOSTNAME_NAME to keep working long-term, set a DHCP reservation for this exact IP"
echo "on the substation router (keyed to this Pi's MAC address) — the script only read the"
echo "current IP, it did not make it static. See 'Give the box a static IP' in deploy/README.md."
echo ""
echo "Verify:"
echo "  On this box:   curl http://127.0.0.1:8080/health"
echo "                 curl -H \"Host: $HOSTNAME_NAME\" http://127.0.0.1/"
echo "  From a LAN PC: nslookup $HOSTNAME_NAME   (should return $BOX_IP)"
echo "                 open http://$HOSTNAME_NAME in a browser"
