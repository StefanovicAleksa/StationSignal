# Deploying to a fixed local hostname (e.g. `http://stationsignal.com`)

Makes the app reachable from every PC on the substation LAN at a fixed, memorable URL, entirely
offline — no real domain registration, no internet dependency. See the top-level `../CLAUDE.md`
for why this is a single-box-per-substation deployment (no multi-site concerns here).

Design: `dnsmasq` on the box resolves `stationsignal.com` to the box's own static IP for every
LAN client; `nginx` on port 80 serves the frontend's production build and reverse-proxies the Go
API's REST + WebSocket routes under that same origin (so the browser never sees CORS or a port
number); `systemd` keeps the API (and the daemon it supervises) running unattended and across
reboots.

```
Other PC's browser → http://stationsignal.com
   (1) DNS: PC's DHCP-assigned DNS = box's static IP → dnsmasq answers stationsignal.com
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
ied_reporter_daemon/rebuild_proj.sh /opt/ied_reporter/bin/ied_reporter_daemon

# API
(cd ied_reporter_api && go build -o /opt/ied_reporter/bin/ied_reporter_api ./cmd/ied_reporter_api)

# Frontend — .env.production (already in the repo) bakes in VITE_API_BASE_URL=http://stationsignal.com
(cd ied_reporter_frontend && npm install && npm run build)
sudo mkdir -p /opt/ied_reporter/frontend-dist
sudo cp -r ied_reporter_frontend/dist/* /opt/ied_reporter/frontend-dist/
```

## 4. Install nginx
```
sudo apt install nginx
sudo cp deploy/nginx/stationsignal.conf /etc/nginx/sites-available/stationsignal.conf
sudo ln -s /etc/nginx/sites-available/stationsignal.conf /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

## 5. Install the systemd service
```
sudo setcap cap_net_raw+ep /opt/ied_reporter/bin/ied_reporter_daemon
sudo useradd --system --no-create-home ied-reporter   # if it doesn't already exist
sudo mkdir -p /opt/ied_reporter/structure_files && sudo chown ied-reporter:ied-reporter /opt/ied_reporter/structure_files
sudo cp deploy/systemd/ied-reporter-api.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ied-reporter-api
```
See the comments in `deploy/systemd/ied-reporter-api.service` — this picks `setcap` on the daemon
binary over running the whole service as root, which resolves the "privilege model for spawning
the daemon" open question in `ied_reporter_api/CLAUDE.md`. Confirm that's acceptable before
relying on it in production.

## Verification
1. On the box: `curl http://127.0.0.1:8080/health` and `curl -H "Host: stationsignal.com" http://127.0.0.1/`.
2. On a second LAN PC: `nslookup stationsignal.com` should return the box's static IP, then open
   `http://stationsignal.com` in a browser and confirm the UI loads, a device report stream
   connects, and a scan runs end-to-end (this is what actually proves the nginx WebSocket proxy
   headers are correct).
3. Reboot the box and repeat step 2 without starting anything by hand, to confirm the systemd
   unit and nginx/dnsmasq's own service units all come back up on their own.
