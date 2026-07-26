# station_signal_api

## Purpose
The Go service between the frontend and `station_signal_daemon` (see `../CLAUDE.md` for how this
fits into the whole product). Two jobs:

1. **Supervise the daemon** — start/stop/monitor the `station_signal_daemon` process, and own its
   raw-socket privilege requirement (GOOSE reception needs root) so the rest of the app doesn't
   have to run privileged.
2. **Relay commands and data** — translate frontend requests into the daemon's control-channel
   JSON, and fan the daemon's report/scan websocket streams back out to connected frontend
   clients.

This file governs `station_signal_api/` only. `../station_signal_daemon/CLAUDE.md` remains the
authority on daemon internals — nothing here should re-derive or fork that.

## Current State
Not started. This folder exists and contains only this file — no `go.mod`, no source yet.

## The Daemon This Service Supervises
Summarized from `../station_signal_daemon/docs/AGENT_API_GUIDE.md` — that file is the full,
self-contained wire contract (confirmed against the daemon's actual code) and is the source of
truth; treat what's below as an orientation, not a substitute for reading it before writing any
client code against it.

- The daemon is a single process, takes no arguments, has no CLI/config file, and does nothing
  until a client sends it a command. `sudo`/root is only required once a command asks it to
  actually talk to a device over GOOSE — the process itself starts and idles fine unprivileged.
- **No REST/HTTP API anywhere** — every interaction, control and data alike, is a websocket
  connection (RFC6455 framing; a raw TCP client won't work). All of the daemon's websockets are
  loopback-only (`127.0.0.1`) and unauthenticated at the transport level (no TLS) — this API is
  the trust boundary the daemon relies on.
- **Control channel — `ws://127.0.0.1:8767`**: the only way to start/stop anything. Bidirectional:
  send `{requestId, action, params}` (`action` is one of `START_REPORTING`, `STOP_REPORTING`,
  `START_SCAN`, `STOP_SCAN`), get back `{schemaVersion, requestId, action, success, result,
  error}`. Multiple clients may connect; every response is **broadcast to all of them**, not just
  the requester — filter on `requestId`. No unsolicited pushes on this channel (e.g. no
  "device disconnected" notification).
- **Per-device report stream — `ws://127.0.0.1:<wsPort>`**: one per device, `wsPort` returned by
  a successful `START_REPORTING`. Push-only stream of normalized MMS report / GOOSE JSON for that
  one device.
- **Scan-result stream — `ws://127.0.0.1:8766`**: one shared instance, live only while at least one
  scan is active. Push-only stream of discovered-device events from an active `START_SCAN`.
- Practical implication for this service: it needs a separate websocket client/connection per
  concern (one to the control channel, one per active device's report stream, one to the scan
  stream when a scan is running) and must manage that connection lifecycle itself — the daemon
  does not multiplex these onto one socket.

## Open Decisions (resolve before/while scaffolding)
Surfaced in planning discussion, not yet decided:
- **Privilege model for spawning the daemon** — does this API process need to run as root itself,
  or should the daemon binary get `setcap cap_net_raw+ep` so an unprivileged API can spawn it
  without the API itself being privileged?
- **One daemon instance per API instance** — the daemon's control channel is a singleton on a
  fixed port (8767), so this API can only supervise one daemon process at a time. Confirm that
  matches the intended deployment (matches the "single box per substation" model in `../CLAUDE.md`).
- **Build vs. prebuilt binary** — does this API expect an already-built `station_signal_daemon`
  binary at a configured path, or does it invoke the daemon's own build (`rebuild_proj.sh`)
  itself? Leaning toward: expects a prebuilt path: build orchestration is a packaging/deploy
  concern, not runtime API logic.
- **Restart-on-crash policy** — the daemon's `device_manager` deliberately keeps no persisted
  state and has no self-health-watcher (removed at explicit user request — see the daemon's
  `CHANGELOG.md`). If the daemon process dies, does this API auto-restart it and need to re-issue
  `START_REPORTING` for every previously-active device, or just surface the crash to the frontend?

## Hard Rules (with reasons)
- **Never re-implement GOOSE/MMS parsing or protocol logic here.** That's the daemon's job, by
  design (see the daemon's own "libiec61850 is mandatory" Hard Rule) — this service only ever
  talks to the daemon over its documented websocket contract.
- **Treat the daemon's JSON envelopes as an external stable contract to consume, not redesign.**
  If a message shape here needs to change, that's a daemon-repo change first, made deliberately —
  not something to work around unilaterally on this side.
