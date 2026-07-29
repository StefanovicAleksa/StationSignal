# Deploying to a fixed local hostname (`http://stationsignal.local`)

Makes the app reachable from every laptop on the substation LAN at a fixed, memorable URL,
entirely offline — no real domain registration, no internet dependency, no DNS or DHCP server to
run. See the top-level `../CLAUDE.md` for why this is a single-box-per-substation deployment (no
multi-site concerns here).

Design: `avahi` (mDNS) on the box answers `stationsignal.local` for any client on the local
segment. This is deliberately not a DNS-server-based setup: the substation's actual client fleet
is technician laptops with hand-configured static IPs (commonly left over from direct IED work
via tools like IEDScout), so a DHCP-based name-resolution handoff can't reach them — they never
send a DHCP request in the first place. mDNS doesn't care how a client got its address, so it
works with zero client-side setup either way. `nginx` on port 80 serves the frontend's production
build and reverse-proxies the Go API's REST + WebSocket routes under that same origin (so the
browser never sees CORS or a port number); `systemd` keeps the API (and the daemon it supervises)
running unattended and across reboots.

```
Other PC's browser → http://stationsignal.local  (or http://<box-ip> directly)
   (1) mDNS: avahi answers stationsignal.local with the box's current interface address — no DNS
       server, no DHCP, no client-side config, works the same for static or dynamic IPs
   (2) HTTP to box:80 → nginx: "/" serves frontend dist/, "/health|/devices|/scans|
       /structure-files|/ws/*" proxy to 127.0.0.1:8080 (the unchanged Go API)
```

## Quick install
Once the box has the toolchains this needs (`gcc`, `go`, `npm`, and `nmcli`/NetworkManager),
`deploy/setup.sh` automates steps 1-5 below: it builds the daemon/API/frontend, detects the box's
current LAN IP/interface/NetworkManager connection, and installs the avahi/nginx configs, the
fixed recovery address + privileged network-config helper, and the systemd service in one run.
```
./deploy/setup.sh
```
Run it **without** `sudo` — it elevates only the specific steps that need root internally (same
convention as `run_dev.sh`). It's safe to re-run after a `git pull` to redeploy.

## 1. Install avahi (mDNS, for `stationsignal.local`)
```
sudo apt install avahi-daemon
```
Set the mDNS-advertised name without renaming the box itself — edit `/etc/avahi/avahi-daemon.conf`
and set, under `[server]`:
```
host-name=stationsignal
```
(avahi always publishes under `.local`, so this alone gives `stationsignal.local` — no separate
domain setting needed, and none should be added: mDNS resolvers only ever look up `.local` per
RFC 6762.) Then:
```
sudo systemctl restart avahi-daemon
```
Because mDNS resolves to whatever address the box's interface currently has, giving the box a
static IP isn't required for `stationsignal.local` to keep working — only for other reasons you
might still want one (predictable SSH access, documentation, and the IED-facing addressing plan
most substations actually need). Setting that first static IP by hand (`nmcli`) remains a normal
OS-level step, outside the scope of this repo — but *changing* it later, once the box is already
deployed and remote, is in scope: see "5. Enable remote network reconfiguration" below.

Known gap: some Android browsers don't resolve `.local` names reliably — those clients fall back
to the bare-IP path (step 3's `default_server`).

## 2. Build everything
```
# Daemon
station_signal_daemon/rebuild_proj.sh /opt/station_signal/bin/station_signal_daemon

# API
(cd station_signal_api && go build -o /opt/station_signal/bin/station_signal_api ./cmd/station_signal_api)

# Frontend — .env.production (already in the repo) intentionally sets no VITE_API_BASE_URL, so
# the built app calls the API on its own page origin (works for stationsignal.local, a bare
# box IP, or anything else nginx answers for — see apiClient.ts)
(cd station_signal_frontend && npm install && npm run build)
sudo mkdir -p /opt/station_signal/frontend-dist
sudo cp -r station_signal_frontend/dist/* /opt/station_signal/frontend-dist/
```

## 3. Install nginx
```
sudo apt install nginx
sudo cp deploy/nginx/stationsignal.conf /etc/nginx/sites-available/stationsignal.conf
sudo ln -s /etc/nginx/sites-available/stationsignal.conf /etc/nginx/sites-enabled/
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t && sudo systemctl reload nginx
```
`stationsignal.conf` is `default_server` (see its own header comment), so it also answers any
request nginx can't match by Host header — including a bare `http://<box-ip>`. This matters
because some clients (e.g. Android phones, whose browsers don't reliably resolve `.local` names)
can still open the app by IP alone, no client-side config needed. Removing the stock `default`
site avoids nginx refusing to start over two conflicting `default_server` entries.

## 4. Install the systemd service
```
sudo setcap cap_net_raw+ep /opt/station_signal/bin/station_signal_daemon
sudo useradd --system --no-create-home station-signal   # if it doesn't already exist
sudo mkdir -p /opt/station_signal/structure_files && sudo chown station-signal:station-signal /opt/station_signal/structure_files
sudo cp deploy/systemd/station-signal-api.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now station-signal-api
```
See the comments in `deploy/systemd/station-signal-api.service` — this picks `setcap` on the daemon
binary over running the whole service as root, which resolves the "privilege model for spawning
the daemon" open question in `station_signal_api/CLAUDE.md`. Confirm that's acceptable before
relying on it in production.

## 5. Enable remote network reconfiguration (Settings page)

The Settings page (`/settings` in the frontend) lets a technician change the box's static IP from
their own laptop's browser — no physical/serial access needed, even when relocating the box to a
substation with a completely different addressing plan. `deploy/setup.sh` automates everything in
this section; it's spelled out here for the manual/step-by-step path and so the safety model is on
the record.

**This assumes NetworkManager (`nmcli`) manages the box's networking** — Raspberry Pi OS
Bookworm's default. If an older image still uses `dhcpcd` (Bullseye and earlier) or
`systemd-networkd`, migrate to NetworkManager first; nothing here supports those.

**The bootstrapping problem this solves**: reaching a box whose current static IP might be wrong
for the network it's just been plugged into is a chicken-and-egg problem — see the top of this
section's design rationale in `station_signal_api/internal/features/network/domain`. The fix is a
second, **permanent, fixed** address every box carries on its LAN interface *in addition to*
whatever primary IP the Settings page manages, established once here and never touched by the
feature itself:

```
sudo nmcli connection modify <connection-name> +ipv4.addresses 169.254.1.1/24
sudo nmcli connection up <connection-name>
```

**This leaves the LAN interface carrying two IPv4 addresses**, and the kernel lists the link-local
one first — so anything resolving an interface name to "its" address and subnet has to choose.
The daemon's scan does exactly that, and prefers the routable address; taking the first one made
it sweep `169.254.1.0/24` and silently find nothing (see `station_signal_daemon/CHANGELOG.md`).
Worth knowing before adding any other code that maps an interface to a subnet.

**Recovering network access** if a box's current primary IP is unknown or unreachable: connect a
laptop directly to the box (or via a switch with nothing else live on that segment), manually set
the laptop's own NIC to a static IP in the same block — e.g. `169.254.1.2/24`, no gateway needed
— then browse to `http://169.254.1.1`. This works regardless of whatever the box's primary IP is
currently set to, with no dependency on DHCP or automatic OS self-addressing (on-site technician
laptops were found to always have hand-configured static IPs already — see the mDNS design note
above — so this manual step matches how technicians already work, rather than relying on
OS auto-negotiation that a statically-addressed laptop would never actually trigger).

**Install the privileged helper** the API shells out to (via a narrowly-scoped `sudo` rule) to
actually apply/confirm/revert a change — this is the *only* place `station_signal_api` reaches
root, mirroring the daemon's own `setcap` grant in spirit rather than running anything as root
persistently:

```
sudo cp deploy/scripts/station-signal-netconfig.sh /opt/station_signal/bin/
sudo chown root:root /opt/station_signal/bin/station-signal-netconfig.sh
sudo chmod 0700 /opt/station_signal/bin/station-signal-netconfig.sh
sudo mkdir -p /etc/station-signal /opt/station_signal/netconfig-state
echo "CONNECTION_NAME=<connection-name>" | sudo tee /etc/station-signal/netconfig.conf

sudo visudo -cf deploy/sudoers/station-signal-netconfig   # validate syntax first — always
sudo install -m 0440 -o root -g root deploy/sudoers/station-signal-netconfig /etc/sudoers.d/
```

**The no-brick guarantee**: submitting a new IP from the Settings page applies it *provisionally*
— the helper snapshots the current config, applies the new one, and schedules an OS-level,
`station-signal-api`-process-independent auto-revert (`systemd-run --on-active=...`) that fires
in 90s (`STATION_SIGNAL_API_NETCONFIG_REVERT_TIMEOUT_SECONDS`, see the systemd unit) unless the
frontend confirms reachability at the new address first. Even if that timed revert somehow never
fires, the fixed `169.254.1.1` address from the step above is untouched by any of this and is
always there as the last resort — there is no scenario where this feature can require physical or
serial access to recover the box.

**The no-wedge guarantee** is the separate one, and it was learned the hard way: the helper's
on-disk "a change is pending" marker refuses any further apply while it exists, and it survives
reboots even though the transient systemd timer meant to clear it does not. A failed activation
whose auto-revert *also* failed once left a box unable to change its own address at all until the
marker was deleted by hand. Four things now prevent that, and `deploy/scripts/tests/run.sh`
exercises every one of them against stubbed `nmcli`/`systemd`:

- Activation names its device explicitly and prefers `nmcli device reapply` over `connection up`,
  so NetworkManager can't pick a different device and fail with "no suitable device found".
- The detached activation runs `station-signal-netconfig.sh activate`, which **reverts itself
  immediately** if activation fails, instead of leaving the box dark until the 90s watchdog.
- `revert` clears its state files from an `EXIT` trap, so a revert that can't reactivate the
  interface still unblocks the next attempt; and a marker past its own `EXPIRES_AT` is treated as
  abandoned rather than authoritative.
- `station-signal-netconfig.sh reconcile` runs at API startup and clears a marker orphaned by a
  reboot or crash.

If a marker somehow still outlives all of that, the Settings page shows a **Clear pending change**
action (`POST /settings/network/revert`) — no shell access needed. By hand, it is
`sudo rm -f /opt/station_signal/netconfig-state/{pending,previous.env}`.

## Raspberry Pi (ARM)

Everything above applies unchanged once the daemon's native libraries are rebuilt for ARM — one
extra step before "2. Build everything", nothing else in this file is architecture-specific
(`go build` has no cgo dependency, and `gcc`/`npm` just target whatever CPU they're running on).

**Why this step exists**: `station_signal_daemon/third_party/lib/*.a` (`libiec61850.a`,
`libhal.a`, `libcjson.a`, `libmxml.a`, `libwebsockets.a`) are committed pre-built x86_64
archives — they will not link on a Pi's ARM. Rebuild them natively, on the Pi itself (no
cross-compilation toolchain needed):

```
git submodule update --init --recursive
station_signal_daemon/setup_project.sh
```

`setup_project.sh` needs `sudo` for its own `apt-get install build-essential cmake git` step —
same as everywhere else in this doc, run it un-escalated and let it prompt. Confirm it actually
produced ARM binaries before trusting the rest of the install:
```
file station_signal_daemon/third_party/lib/*.a   # should say ARM, not x86-64
```
This overwrites the committed archives in your working tree — normal and expected for a Pi
checkout, but don't `git add`/commit that change back onto a branch other machines also build
from (see `station_signal_daemon/CLAUDE.md`'s "Don't touch `third_party/`" Hard Rule — this is
the one sanctioned exception, and even then only for the machine that genuinely needs it).

From here, continue at "2. Build everything" (or just run `./deploy/setup.sh`, which now builds
against the freshly-rebuilt ARM libraries) exactly as above. Two Pi-specific things to watch for:

- **avahi-daemon is usually already installed and running** on Raspberry Pi OS by default
  (that's what makes a stock Pi answer to `raspberrypi.local`) — step 1 above still applies, just
  edit the existing `/etc/avahi/avahi-daemon.conf` instead of installing the package fresh.
- **Node.js version**: the frontend's `package.json` requires `^22.18.0 || >=24.12.0`. Raspberry
  Pi OS's `apt` Node is often older than that — if `npm install`/`npm run build` complains, install
  a current Node via [NodeSource's ARM builds](https://github.com/nodesource/distributions) or
  `nvm` rather than the distro package.

## Verification
1. On the box: `curl http://127.0.0.1:8080/health` and `curl -H "Host: stationsignal.local" http://127.0.0.1/`.
2. On a second LAN PC (a statically-addressed one is the normal case here): open
   `http://stationsignal.local` in a browser and confirm the UI loads, a device report stream
   connects, and a scan runs end-to-end (this is what actually proves the nginx WebSocket proxy
   headers are correct).
3. On any device where mDNS doesn't resolve (e.g. an Android phone): open `http://<box-ip>`
   directly. Because `stationsignal.conf` is `default_server`, this should load the app the same
   as the hostname does — no config required. If it instead shows nginx's stock "Welcome to
   nginx" page, `sudo rm -f /etc/nginx/sites-enabled/default` and reload nginx (`deploy/setup.sh`
   does this automatically).
4. Reboot the box and repeat step 2 without starting anything by hand, to confirm the systemd
   unit and nginx/avahi's own service units all come back up on their own.
5. Confirm the fixed recovery address is live and independent of the box's primary IP:
   `ping 169.254.1.1` from a laptop with a manually-set `169.254.x.x/24` address on the same
   segment, then open `http://169.254.1.1` and confirm the Settings page loads. Then, on the
   Settings page, submit a deliberately unreachable IP and confirm the box auto-reverts and
   becomes reachable again at its original address without any manual intervention within the
   configured timeout — this is the one guarantee that must never fail before this feature is
   trusted on a field-deployed box.
