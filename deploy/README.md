# Deploying to a fixed local hostname (e.g. `http://stationsignal.internal`)

Makes the app reachable from every PC on the substation LAN at a fixed, memorable URL, entirely
offline — no real domain registration, no internet dependency. See the top-level `../CLAUDE.md`
for why this is a single-box-per-substation deployment (no multi-site concerns here).

Uses `.internal` (RFC 9476), not a real public TLD like `.com` — that space is reserved and
guaranteed never delegated as a live domain. An earlier revision used `stationsignal.com`, which
turned out to collide with an unrelated real, HSTS-enabled website of the same name: any browser
that had ever touched the real site (or ships it HSTS-preloaded) silently force-upgraded every
request here to HTTPS, which this deliberately-plain-HTTP nginx has no way to answer — and there
was no reliable per-client fix. Don't repoint this at another real-feeling domain without checking
it isn't actually registered.

Design: `dnsmasq` on the box resolves `stationsignal.internal` to the box's own static IP for every
LAN client; `nginx` on port 80 serves the frontend's production build and reverse-proxies the Go
API's REST + WebSocket routes under that same origin (so the browser never sees CORS or a port
number); `systemd` keeps the API (and the daemon it supervises) running unattended and across
reboots.

```
Other PC's browser → http://stationsignal.internal
   (1) DNS: PC's DHCP-assigned DNS = box's static IP → dnsmasq answers stationsignal.internal
   (2) HTTP to box:80 → nginx: "/" serves frontend dist/, "/health|/devices|/scans|
       /structure-files|/ws/*" proxy to 127.0.0.1:8080 (the unchanged Go API)
```

## Quick install
Once the box has the toolchains this needs (`gcc`, `go`, `npm`) and you've read the static-IP
note in step 1 below, `deploy/setup.sh` automates steps 2-5: it builds the daemon/API/frontend,
detects the box's current LAN IP and upstream DNS, and installs the dnsmasq/nginx configs and
the systemd service in one run.
```
./deploy/setup.sh
```
Run it **without** `sudo` — it elevates only the specific steps that need root internally (same
convention as `run_dev.sh`). It's safe to re-run after a `git pull` to redeploy. It deliberately
does **not** touch the box's network config or the router — that stays manual (step 1 below), and
the script prints the detected IP as a reminder to set a matching router DHCP reservation. If it
aborts on a preflight check (missing toolchain, something already on port 53), the step-by-step
sections below explain what it's checking for and how to resolve it.

## 1. Give the box a static IP
Outside the scope of this repo — depends on the box's OS/network stack (netplan/NetworkManager
on Debian/Ubuntu, or a DHCP reservation on the substation router keyed to the box's MAC). Do this
first; everything below assumes the box's IP won't change.

## 2. Install dnsmasq
```
sudo apt install dnsmasq   # or the box's equivalent
```
Before installing the config, check whether something is already resolving DNS on the box on
port 53 (`systemd-resolved`, NetworkManager's built-in dnsmasq) — a second listener on the same
port will fail to bind. If `systemd-resolved` owns port 53, either disable its stub listener or
point dnsmasq at a different local port and forward accordingly; if NetworkManager runs its own
dnsmasq, drop the config into `/etc/NetworkManager/dnsmasq.d/` instead of `/etc/dnsmasq.d/`.

Edit `deploy/dnsmasq/stationsignal.conf`, filling in the box's real static IP and the upstream
DNS IP (usually the router), then:
```
sudo cp deploy/dnsmasq/stationsignal.conf /etc/dnsmasq.d/stationsignal.conf
sudo systemctl restart dnsmasq
```
Then either point the substation router's DHCP "DNS server" setting at the box's static IP (all
LAN clients pick it up on next DHCP renewal — preferred, zero per-PC work), or set each PC's DNS
server to the box's IP manually if router DHCP isn't accessible to you.

## 3. Build everything
```
# Daemon
station_signal_daemon/rebuild_proj.sh /opt/station_signal/bin/station_signal_daemon

# API
(cd station_signal_api && go build -o /opt/station_signal/bin/station_signal_api ./cmd/station_signal_api)

# Frontend — .env.production (already in the repo) intentionally sets no VITE_API_BASE_URL, so
# the built app calls the API on its own page origin (works for stationsignal.internal, a bare
# box IP, or anything else nginx answers for — see apiClient.ts)
(cd station_signal_frontend && npm install && npm run build)
sudo mkdir -p /opt/station_signal/frontend-dist
sudo cp -r station_signal_frontend/dist/* /opt/station_signal/frontend-dist/
```

## 4. Install nginx
```
sudo apt install nginx
sudo cp deploy/nginx/stationsignal.conf /etc/nginx/sites-available/stationsignal.conf
sudo ln -s /etc/nginx/sites-available/stationsignal.conf /etc/nginx/sites-enabled/
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t && sudo systemctl reload nginx
```
`stationsignal.conf` is `default_server` (see its own header comment), so it also answers any
request nginx can't match by Host header — including a bare `http://<box-ip>`. This matters
because the substation router's DHCP/DNS settings are often outside our control (client-owned
network), so clients that never picked up `stationsignal.internal` via DNS (or can't — e.g. many
Android phones don't resolve mDNS `.local` names in the browser either) can still open the app by
IP alone, no client-side config needed. Removing the stock `default` site avoids nginx refusing to
start over two conflicting `default_server` entries.

## 5. Install the systemd service
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

## Raspberry Pi (ARM)

Everything above applies unchanged once the daemon's native libraries are rebuilt for ARM — one
extra step before "3. Build everything", nothing else in this file is architecture-specific
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

From here, continue at "3. Build everything" (or just run `./deploy/setup.sh`, which now builds
against the freshly-rebuilt ARM libraries) exactly as above. Two Pi-specific things to watch for:

- **Port 53**: Raspberry Pi OS Bookworm's NetworkManager ships its own embedded dnsmasq bound to
  port 53, the same conflict class as `systemd-resolved` on Ubuntu (step 2 above) — check
  `/etc/NetworkManager/dnsmasq.d/` instead of `/etc/dnsmasq.d/` if so. If the Pi (like a dev
  workstation) also needs to keep other local DNS-dependent services working, scope the
  `stationsignal.internal` dnsmasq config to the LAN interface only instead of disabling the
  competing resolver outright: add `interface=<lan-iface>`, `bind-interfaces`, and
  `except-interface=lo` to `deploy/dnsmasq/stationsignal.conf` before installing it.
- **Node.js version**: the frontend's `package.json` requires `^22.18.0 || >=24.12.0`. Raspberry
  Pi OS's `apt` Node is often older than that — if `npm install`/`npm run build` complains, install
  a current Node via [NodeSource's ARM builds](https://github.com/nodesource/distributions) or
  `nvm` rather than the distro package.

## Verification
1. On the box: `curl http://127.0.0.1:8080/health` and `curl -H "Host: stationsignal.internal" http://127.0.0.1/`.
2. On a second LAN PC: `nslookup stationsignal.internal` should return the box's static IP, then open
   `http://stationsignal.internal` in a browser and confirm the UI loads, a device report stream
   connects, and a scan runs end-to-end (this is what actually proves the nginx WebSocket proxy
   headers are correct).
3. On any device (including one where DNS was never pointed at the box, e.g. a phone on a client
   network you can't reconfigure): open `http://<box-ip>` directly. Because `stationsignal.conf`
   is `default_server`, this should load the app the same as the hostname does — no DNS setup
   required. If it instead shows nginx's stock "Welcome to nginx" page, `sudo rm -f
   /etc/nginx/sites-enabled/default` and reload nginx (`deploy/setup.sh` does this automatically).
4. Reboot the box and repeat step 2 without starting anything by hand, to confirm the systemd
   unit and nginx/dnsmasq's own service units all come back up on their own.
