# frontend

## Purpose
What the technician actually looks at on-site: live GOOSE/MMS data, device/scan controls (see
`../CLAUDE.md` for how this fits into the whole product). Talks to `station_signal_api` only —
never to `station_signal_daemon` directly, and never over anything but the REST/WS surface that
API exposes.

This file governs `frontend/` only. `../station_signal_api/CLAUDE.md` remains the authority on
the API's own internals — nothing here should re-derive or fork that.

## Current State
Scaffolded, no application code yet. Created via `create-vue` (Vue 3 + TypeScript + Vue Router
+ Pinia + Vitest, `--bare` — no example/demo code), dependencies installed, `pnpm build`
verified working. `src/App.vue`, `src/router/index.ts`, `src/stores/counter.ts` are all
template boilerplate from the scaffold, not yet adapted to this app.

## The API This Frontend Talks To
Read `../station_signal_api/docs/FRONTEND_API_GUIDE.md` **before writing any client code** — it
is the full, self-contained wire contract (every REST endpoint, both WS endpoints, every error
code, worked flows) confirmed directly against that API's actual code. Treat what's below as
orientation, not a substitute for reading it.

- Base URL is this API's own HTTP address (e.g. `http://<host>:8080`) — **never** the daemon's
  ports (8767/8766/9000-9999). This frontend has no reason to know those exist.
- Two REST resource groups, `/devices` and `/scans` (`POST` to start, `DELETE /{id}` to stop,
  `GET` to list), plus `GET /health`.
- Two WS endpoints for live data: `/ws/devices/{deviceId}` (one connection per watched
  device) and `/ws/scans` (one shared connection, accepts and idles even with no scan active).
  Both are push-only — never send anything on them.
- Every error response is `{"error":{"code":"...","message":"...","stage":null,"detail":null}}`
  — the guide's §5 has the full code → HTTP-status table, including which ones (`DAEMON_UNREACHABLE`)
  deserve a retry-with-backoff UI treatment rather than a hard failure.
- No auth, no TLS anywhere in this stack (loopback/local-substation-network trust model per
  the top-level `../CLAUDE.md`) — don't build auth flows this system doesn't have.

## Hard Rules (with reasons)
- **Never connect to the daemon's own ports.** This frontend's only backend is
  `station_signal_api` — even though the daemon's raw wire format is technically reachable if
  you knew the ports, going around the API defeats the entire reason it exists (privilege
  separation, connection multiplexing, crash re-arm). If something feels missing from the
  API's surface, that's an API change to make deliberately, not something to work around here.
- **Treat `FRONTEND_API_GUIDE.md` as an external stable contract to consume, not guess at.**
  If the API's response shape ever seems to need a change, that's a `station_signal_api` change
  first — not something to paper over with ad-hoc parsing on this side.
