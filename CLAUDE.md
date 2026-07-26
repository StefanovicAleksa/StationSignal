# ied_reporter

## Purpose
A real-time IEC 61850 GOOSE/MMS monitoring tool for substation engineers and technicians, used
during commissioning, testing, and troubleshooting of IEDs. Manually inspecting GOOSE/MMS traffic
with a generic packet-capture tool is slow and requires reading raw protocol structure by hand;
this app instead gives a live, normalized, human-readable view of what an IED is actually
reporting — GOOSE frames and MMS report control block updates alike, decoded and streamed as they
happen. Deployment target is a single on-site box per substation: daemon, API, and frontend all
run together on the same network as the IEDs being monitored, for one engineer/technician working
one substation at a time. No multi-site, multi-tenant, or remote/cloud monitoring concerns are in
scope today.

## Repo Layout
Three sibling projects, each with its own `CLAUDE.md` governing its internals — this file only
covers what ties them together, not how any one of them works.

- **`ied_reporter_daemon/`** — implemented. C. Sniffs GOOSE off the wire and subscribes to MMS
  report control blocks on target IEDs, normalizes both into JSON, and exposes everything over
  three local, loopback-only websockets: a control channel to start/stop reporting on a device or
  scanning the local network for IEDs, and two push-only streams (per-device MMS/GOOSE reports,
  discovered-device results). See `ied_reporter_daemon/CLAUDE.md` for the full command/websocket
  contract and internal architecture.
- **`ied_reporter_api/`** — implemented. Go. Two jobs: supervise the daemon process (start/
  stop/monitor it, own its raw-socket privilege requirement so the rest of the app doesn't
  need to run as root), and relay commands between the frontend and the daemon's websockets —
  translating frontend requests into the daemon's control-channel JSON, and fanning the daemon's
  report/scan streams back out to connected frontend clients over its own REST + WebSocket API.
  See `ied_reporter_api/CLAUDE.md` and `ied_reporter_api/docs/API_OVERVIEW.md` for internals,
  and `ied_reporter_api/docs/FRONTEND_API_GUIDE.md` for the wire contract the frontend builds
  against.
- **`frontend/`** — scaffolded, no application code yet. Vue 3 + TypeScript + Vue Router +
  Pinia + Vitest. What the technician actually looks at on-site: live GOOSE/MMS data,
  device/scan controls. See `frontend/CLAUDE.md`.

## How the Pieces Fit Together
On one box, at one substation: the technician opens the frontend → the frontend talks to the Go
API → the API supervises and talks to the daemon over its three local websockets (control / IPC /
scan) → the daemon sniffs GOOSE and subscribes to MMS reports from IEDs on that substation's local
network → normalized JSON flows back up through the API to the technician's screen in real time.
Because this is a single-box, single-substation deployment, the API and frontend should not carry
multi-site/remote-network assumptions (e.g. reaching IEDs across a WAN, or one API instance
managing several physically separate substations) — that's a deliberate scope boundary, not an
oversight, unless a future decision explicitly changes it.

## Current State
- **Daemon**: fully implemented and proven end-to-end (see `ied_reporter_daemon/CLAUDE.md` and its
  `CHANGELOG.md` for the complete feature set and history).
- **API**: implemented (feature-sliced domain/data/service layers), with a unit + integration
  test suite (the latter against the real daemon binary) and full docs — see
  `ied_reporter_api/CLAUDE.md`.
- **Frontend**: scaffolded (Vue 3 + TypeScript + Vue Router + Pinia + Vitest), no application
  code yet — see `frontend/CLAUDE.md`.

Update this section as each piece progresses, same convention as the daemon's own `CLAUDE.md`.
