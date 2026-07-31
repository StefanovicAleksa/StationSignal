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
# Fixed, permanent secondary address every box carries on its LAN interface, independent of
# whatever primary static IP the Settings page later manages — see the "Recovering network
# access" section of deploy/README.md and internal/features/network/domain.RecoveryAddress
# (station_signal_api), which this must be kept in sync with by hand.
RECOVERY_ADDR="169.254.1.1"
RECOVERY_CIDR="$RECOVERY_ADDR/24"

echo "==> Preflight: checking required tools"
for tool in gcc go npm curl sudo ip sed nmcli visudo; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: '$tool' is required but not found on PATH" >&2; exit 1; }
done

echo "==> Detecting this box's LAN IP and interface (read-only — no network config is changed yet)"
BOX_IP="${BOX_IP:-$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if ($i=="src") print $(i+1)}')}"
BOX_IFACE="${BOX_IFACE:-$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if ($i=="dev") print $(i+1)}')}"
if [ -z "$BOX_IP" ] || [ -z "$BOX_IFACE" ]; then
    echo "error: could not auto-detect this box's LAN IP/interface. Re-run with BOX_IP=<ip>" >&2
    echo "       BOX_IFACE=<iface> ./deploy/setup.sh" >&2
    exit 1
fi
echo "    box IP:        $BOX_IP"
echo "    box interface: $BOX_IFACE"

echo "==> Detecting the NetworkManager connection profile for $BOX_IFACE"
CONNECTION_NAME="${CONNECTION_NAME:-$(nmcli -g GENERAL.CONNECTION device show "$BOX_IFACE" 2>/dev/null)}"
if [ -z "$CONNECTION_NAME" ] || [ "$CONNECTION_NAME" = "--" ]; then
    echo "error: could not find an active NetworkManager connection on $BOX_IFACE." >&2
    echo "       This feature assumes NetworkManager (nmcli) manages the box's networking —" >&2
    echo "       Raspberry Pi OS Bookworm's default. Re-run with CONNECTION_NAME=<name>" >&2
    echo "       ./deploy/setup.sh if it's managed under a different name, or configure" >&2
    echo "       NetworkManager first if something else (dhcpcd, systemd-networkd) owns $BOX_IFACE." >&2
    exit 1
fi
echo "    connection:    $CONNECTION_NAME"

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

echo "==> Creating log directory /var/log/station_signal"
sudo mkdir -p /var/log/station_signal
sudo chown "$SERVICE_USER:$SERVICE_USER" /var/log/station_signal

echo "==> Adding the fixed recovery address ($RECOVERY_CIDR) to $CONNECTION_NAME"
# Permanent and independent of whatever primary IP the Settings page manages later — this is
# the guaranteed-reachable fallback if a remote IP change ever goes wrong. `+ipv4.addresses`
# appends rather than replaces; nmcli silently no-ops if it's already present, so this is safe
# to re-run.
#
# Also force ipv4.method=manual explicitly here rather than assuming it's already set: this
# connection is expected to already be statically configured before setup.sh ever runs, but if
# it isn't (or drifted back to "auto" — e.g. after a manual `ip addr` intervention that bypassed
# NetworkManager, or an interrupted netconfig apply/revert cycle), `nmcli connection up` below
# would otherwise silently retry DHCP against a network with no DHCP server for up to 45s per
# attempt, forever, with a confusing "IP configuration could not be reserved" error. Cheap to
# assert unconditionally; a no-op if it's already manual.
sudo nmcli connection modify "$CONNECTION_NAME" ipv4.method manual +ipv4.addresses "$RECOVERY_CIDR"
# Pin the device, same as station-signal-netconfig.sh's activate_profile does and for the same
# reason: a bare `nmcli connection up <name>` lets NetworkManager choose the device, and it can
# choose a different one and then fail outright ("No suitable device found for this connection
# (device <other> not available because profile is not compatible with device (mismatching
# interface name))"). That failure here would abort setup.sh under `set -e` partway through the
# install, with the interface already reconfigured — the worst possible moment for it.
sudo nmcli connection up "$CONNECTION_NAME" ifname "$BOX_IFACE" >/dev/null

echo "==> Installing the privileged network-config helper (Settings page IP changes)"
sudo cp "$DEPLOY_DIR/scripts/station-signal-netconfig.sh" "$INSTALL_ROOT/bin/station-signal-netconfig.sh"
sudo chown root:root "$INSTALL_ROOT/bin/station-signal-netconfig.sh"
sudo chmod 0700 "$INSTALL_ROOT/bin/station-signal-netconfig.sh"
sudo mkdir -p /etc/station-signal "$INSTALL_ROOT/netconfig-state"
# %q, not %s: station-signal-netconfig.sh sources this file, and NetworkManager's own default
# connection name ("Wired connection 1") has spaces — unquoted, that breaks the sourced
# assignment into a bogus command invocation ("connection: command not found").
printf 'CONNECTION_NAME=%q\n' "$CONNECTION_NAME" | sudo tee /etc/station-signal/netconfig.conf >/dev/null
# Always validate with `visudo -cf` against a staged copy before touching /etc/sudoers.d — a
# malformed sudoers file can lock out sudo entirely.
STAGED_SUDOERS="$(mktemp)"
cp "$DEPLOY_DIR/sudoers/station-signal-netconfig" "$STAGED_SUDOERS"
sudo visudo -cf "$STAGED_SUDOERS"
sudo install -m 0440 -o root -g root "$STAGED_SUDOERS" /etc/sudoers.d/station-signal-netconfig
rm -f "$STAGED_SUDOERS"

echo "==> Installing avahi (mDNS) so $HOSTNAME_NAME resolves"
command -v avahi-daemon >/dev/null 2>&1 || sudo apt-get install -y avahi-daemon
if grep -q '^host-name=' /etc/avahi/avahi-daemon.conf; then
    sudo sed -i "s/^host-name=.*/host-name=$AVAHI_HOST_NAME/" /etc/avahi/avahi-daemon.conf
elif grep -q '^#host-name=' /etc/avahi/avahi-daemon.conf; then
    sudo sed -i "s/^#host-name=.*/host-name=$AVAHI_HOST_NAME/" /etc/avahi/avahi-daemon.conf
else
    sudo sed -i "/^\[server\]/a host-name=$AVAHI_HOST_NAME" /etc/avahi/avahi-daemon.conf
fi
# Stop avahi auto-publishing an A record for every address on every interface — this box's LAN
# interface always carries two (its real IP, plus the fixed recovery address below), and avahi's
# default here made $HOSTNAME_NAME resolve to *both*. A client that raced/tried the recovery
# address first got no response (it's not on that segment) and stalled through a TCP connect
# timeout — ~10s — before falling back to the address that actually works. Publish exactly one,
# by hand, via the static /etc/avahi/hosts mechanism instead — kept in sync with the box's actual
# live primary address by station-signal-netconfig.sh's sync_avahi_hosts (see its own comment;
# this is the same kind of hand-maintained cross-reference as RECOVERY_CIDR above).
if grep -q '^publish-addresses=' /etc/avahi/avahi-daemon.conf; then
    sudo sed -i "s/^publish-addresses=.*/publish-addresses=no/" /etc/avahi/avahi-daemon.conf
elif grep -q '^#publish-addresses=' /etc/avahi/avahi-daemon.conf; then
    sudo sed -i "s/^#publish-addresses=.*/publish-addresses=no/" /etc/avahi/avahi-daemon.conf
else
    sudo sed -i "/^\[publish\]/a publish-addresses=no" /etc/avahi/avahi-daemon.conf
fi
AVAHI_HOSTS_MARKER_BEGIN="# station-signal: managed entry, do not edit by hand (see deploy/setup.sh / deploy/scripts/station-signal-netconfig.sh)"
AVAHI_HOSTS_MARKER_END="# station-signal: end managed entry"
sudo touch /etc/avahi/hosts
sudo sed -i "\|^$AVAHI_HOSTS_MARKER_BEGIN\$|,\|^$AVAHI_HOSTS_MARKER_END\$|d" /etc/avahi/hosts
{
    echo "$AVAHI_HOSTS_MARKER_BEGIN"
    echo "$BOX_IP $HOSTNAME_NAME"
    echo "$AVAHI_HOSTS_MARKER_END"
} | sudo tee -a /etc/avahi/hosts >/dev/null
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
echo "API + daemon logs (both interleaved): /var/log/station_signal/station-signal-api.log"
echo ""
echo "Verify:"
echo "  On this box:   curl http://127.0.0.1:8080/api/health"
echo "                 curl -H \"Host: $HOSTNAME_NAME\" http://127.0.0.1/"
echo "  From a LAN PC: open http://$HOSTNAME_NAME in a browser"
echo ""
echo "Fixed recovery address: http://$RECOVERY_ADDR (always reachable, never changed by the"
echo "Settings page) — see 'Recovering network access' in deploy/README.md if a remote IP"
echo "change ever needs to be undone by hand."
