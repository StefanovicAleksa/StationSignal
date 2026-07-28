# station_signal

A real-time IEC 61850 GOOSE/MMS monitoring tool for substation engineers and technicians, used
during commissioning, testing, and troubleshooting of IEDs (Intelligent Electronic Devices).

Manually inspecting GOOSE/MMS traffic with a generic packet-capture tool is slow and requires
reading raw protocol structure by hand. This app instead gives a live, normalized, human-readable
view of what an IED is actually reporting — GOOSE frames and MMS report control block updates
alike, decoded and streamed as they happen.

Deployment target is a single on-site box per substation: daemon, API, and frontend all run
together on the same network as the IEDs being monitored, for one or more engineers/technicians
working one substation at a time — each browser/session sees and controls only the scans and
devices it started itself, so multiple technicians can use the tool concurrently without
interfering with each other. No multi-site, multi-tenant, or remote/cloud monitoring is in scope.

## How it works

Three pieces, one box, three local pipes between them:

```
Technician's browser
      │  HTTP + WebSocket
      ▼
station_signal_frontend  (Vue 3 UI)
      │  REST + WebSocket
      ▼
station_signal_api  (Go)
      │  three loopback-only websockets (control / IPC / scan)
      ▼
station_signal_daemon  (C)
      │  sniffs GOOSE off the wire, subscribes to MMS reports
      ▼
IEDs on the substation LAN
```

- **`station_signal_daemon/`** (C) — sniffs GOOSE off the wire and subscribes to MMS report
  control blocks on target IEDs, normalizes both into JSON, and exposes everything over three
  local, loopback-only websockets: a control channel to start/stop reporting on a device or
  scanning the local network for IEDs, and two push-only streams (per-device MMS/GOOSE reports,
  discovered-device results).
- **`station_signal_api/`** (Go) — supervises the daemon process (start/stop/monitor it, owning
  its raw-socket privilege requirement so nothing else needs to run as root), and relays commands
  between the frontend and the daemon's websockets over its own REST + WebSocket API.
- **`station_signal_frontend/`** (Vue 3 + TypeScript) — what the technician actually looks at
  on-site: live GOOSE/MMS data, device/scan controls, network settings.

Each piece has its own `CLAUDE.md` governing its internals in depth; the root `CLAUDE.md` covers
only what ties them together.

## Repo layout

```
station_signal_daemon/     C daemon — GOOSE sniffing + MMS reporting, three websockets
station_signal_api/        Go API — supervises the daemon, serves REST + WebSocket to the frontend
station_signal_frontend/   Vue 3 frontend — the technician's UI
deploy/                    Production install: avahi (mDNS), nginx, systemd, network-settings helper
run_dev.sh                 Builds + runs all three pieces together for local development
run_simulated_ieds.sh      Spins up fake IEDs on a dummy interface for manual frontend testing
```

## Setup — Linux (development)

Use this to run the app from source on your own Linux machine (not a production install).

**Prerequisites**
- `gcc` — builds the daemon
- `go` — builds the API
- `node`/`npm` — the frontend needs Node `^22.18.0` or `>=24.12.0`
- `curl`, `sudo`

The daemon's third-party libraries (`libiec61850`, etc.) come pre-built for x86_64 in
`station_signal_daemon/third_party/` — no submodules or extra build step needed unless you're on
a different architecture (see the Raspberry Pi section below).

**Run everything**
```sh
git clone <your-remote-url>
cd station_signal
./run_dev.sh
```
This builds the daemon and API, installs frontend dependencies if needed, then starts all three:
the API (which spawns the daemon as a child, under `sudo` — real GOOSE reception needs
`CAP_NET_RAW`) and the frontend dev server. It prints the API and frontend URLs (frontend
defaults to `http://localhost:5173`) and tears everything down cleanly on Ctrl+C.

Run it **without** `sudo` — it elevates only the API/daemon sub-process internally. Running the
whole script as root would put `npm run dev` under sudo's sanitized `PATH`, often resolving to a
different (too-old) Node.

**Optional: simulated IEDs**, so the frontend's Network Scan page has something real to discover
without needing an actual substation:
```sh
./run_simulated_ieds.sh        # spins up 3 fake IEDs by default; needs run_dev.sh running separately
```

**Tests** — see each project's own `CLAUDE.md`/`README.md` (`station_signal_daemon/run_all_tests.sh`,
`station_signal_api`'s Go test suite, `station_signal_frontend`'s Vitest suite).

## Setup — Raspberry Pi (production deployment)

This installs the app as a persistent, always-on service reachable from any laptop on the
substation LAN at `http://stationsignal.local` — the intended field deployment. Full detail,
including the network-recovery/no-brick design, is in `deploy/README.md`; this is the condensed
path.

**Prerequisites**
- Raspberry Pi OS **Bookworm** (needed for NetworkManager/`nmcli`, which the network-settings
  feature depends on — older images using `dhcpcd`/`systemd-networkd` aren't supported)
- SSH enabled — either via Raspberry Pi Imager's settings before first boot, or by dropping an
  empty `ssh` file in the boot partition
- `gcc`, `go`, `npm`, `nmcli` installed on the Pi (Node must satisfy `^22.18.0 || >=24.12.0` — the
  Raspberry Pi OS `apt` Node is often older; use NodeSource's ARM builds or `nvm` instead)

**Steps**
```sh
# 1. On the Pi, over SSH:
git clone <your-remote-url>
cd station_signal

# 2. Rebuild the daemon's third-party libraries natively for ARM — the committed archives are
#    prebuilt x86_64 and won't link on a Pi.
git submodule update --init --recursive
station_signal_daemon/setup_project.sh
file station_signal_daemon/third_party/lib/*.a   # sanity check: should say ARM, not x86-64

# 3. Build + install everything (daemon, API, frontend, avahi, nginx, systemd service,
#    fixed recovery address, network-settings helper) in one run.
./deploy/setup.sh
```
Run `deploy/setup.sh` **without** `sudo` — same reasoning as `run_dev.sh` above, it elevates only
the specific steps that need root. It's safe to re-run after a `git pull` to redeploy.

Don't commit the ARM-rebuilt `third_party/*.a` archives back onto a branch other (x86_64)
machines build from — this is a Pi-only working-tree change.

**Verify**
1. On the Pi: `curl http://127.0.0.1:8080/health`
2. From another machine on the LAN: open `http://stationsignal.local` in a browser and confirm
   the UI loads, a device report stream connects, and a scan runs end-to-end.
3. Reboot the Pi and repeat step 2 without starting anything by hand, to confirm the systemd
   unit, nginx, and avahi all come back up on their own.

See `deploy/README.md` for the full verification checklist, the fixed `169.254.1.1` recovery
address (always reachable if a remote IP change ever goes wrong), and the manual step-by-step
version of everything `deploy/setup.sh` automates.

**Making changes afterward**: SSH into the Pi (`ssh pi@stationsignal.local`), edit code, then
re-run `./deploy/setup.sh` to rebuild and redeploy in place.
