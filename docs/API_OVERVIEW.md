# ied_reporter_api — Overview

The Go service between the frontend and `ied_reporter_daemon` (see `../CLAUDE.md` and the
top-level `../../CLAUDE.md` for how this fits into the whole product). Two jobs: supervise the
daemon process (own its raw-socket privilege requirement so nothing else has to run
privileged), and relay commands/data between the frontend and the daemon's three
loopback-only websockets. This doc is the map — for current-state facts and hard rules see
`CLAUDE.md` at this repo's root; for the wire contract a frontend builds against, see
`FRONTEND_API_GUIDE.md`, which is self-contained and assumes no access to this source tree.

## What it does, end to end

1. Spawns the configured `ied_reporter_daemon` binary and supervises it — detects unexpected
   exit, respawns with backoff, and re-arms every device/scan that was active immediately
   before a crash.
2. Exposes REST endpoints (`/devices`, `/scans`, `/health`) that translate frontend requests
   into the daemon's control-channel JSON (`START_REPORTING`/`STOP_REPORTING`/`START_SCAN`/
   `STOP_SCAN`) over one shared, long-lived connection to `ws://127.0.0.1:8767`. Also exposes
   `POST /structure-files`, a local-only file upload endpoint with no daemon interaction — see
   `internal/core/structurefiles` below.
3. Exposes WS endpoints (`/ws/devices/{id}`, `/ws/scans`) that fan the daemon's per-device
   report stream and shared scan-result stream out to any number of frontend subscribers —
   the frontend never dials the daemon's own ports directly.

## Architecture: feature-sliced, with one core exempt from the split

Each business capability lives under `internal/features/<name>/`, split into three layers —
mirroring the daemon's own `domain`/`data`/`service` convention, confirmed against its actual
`#include` graph rather than assumed:

- **`domain/`** — pure structs/enums/value types + pure logic functions. No I/O, no
  third-party imports, no dependency on this feature's own `data/`/`service/`.
- **`data/`** — the concrete mechanics: talking to the daemon (via `core/daemonclient`),
  in-memory state (mutex-guarded stores). Imports `domain/` for types; never imported by it.
- **`service/`** — the single public façade. Imports both `domain/` and `data/`, wires them
  together, and is the only thing external callers (controllers, `main`, other features) are
  allowed to depend on for this feature's behavior — `domain/` types are also legitimately
  part of the public contract, since service methods take/return them.

| Feature | Job |
|---|---|
| [`reporting`](features/reporting.md) | `START_REPORTING`/`STOP_REPORTING` + per-device stream hub lifecycle |
| [`scanning`](features/scanning.md) | `START_SCAN`/`STOP_SCAN` + the shared scan-result hub's 0↔1 lifecycle |
| [`supervision`](features/supervision.md) | daemon child-process lifecycle: spawn, crash detection, backoff respawn |

`internal/core/` is a top-level sibling of `internal/features/`, exempt from the
domain/data/service split, holding what every feature's `data/` layer depends on:

| Package | Job |
|---|---|
| `daemonproto` | Go structs for the daemon's JSON wire contract (control envelopes, stream messages, error codes) |
| `daemonclient` | the single long-lived control-channel connection, request/response correlation by `requestId` |
| `streamrelay` | `Hub`: fans one daemon-side push-only stream out to N frontend subscribers, drop-not-queue on backpressure |
| `config` | startup flags/env (daemon binary path, HTTP listen address, log level, structure file storage dir) |
| `structurefiles` | saves uploaded SCL/ICD/CID files to local disk for `POST /structure-files`; no daemon interaction — the returned path is read by the daemon directly, since it always runs on the same box |

`internal/controllers/{rest,ws}` is the HTTP/WS handler layer — not itself a "feature" — that
calls into each feature's `service/` only, translating REST/WS requests into service calls and
domain types into JSON.

## Crash re-arm

The daemon persists nothing across a restart and has no self-health-watcher by design (see the
daemon's own `CHANGELOG.md`). This API is the only place that remembers what was active:

1. `supervision.Supervisor` detects the daemon process exiting unexpectedly, respawns it with
   exponential backoff, and signals `Restarts()` once the new process's control channel is
   accepting connections.
2. `core/daemonclient.Client` (the sole consumer of `Restarts()`) redials, and signals its own
   `Reconnects()` once a `Call` is actually guaranteed to work — not merely once the process
   is up.
3. `main`'s re-arm goroutine watches `Reconnects()`, skips the first (startup) signal, and on
   every subsequent one calls each feature service's `Snapshot()` (what was active) →
   `Clear()` (drop the now-dead state/hubs) → `Start()` per snapshot entry (replay against the
   fresh daemon) — using only each feature's public `service/` API, never reaching into its
   `data/` layer directly.

## `main.go`'s wiring order

`cmd/ied_reporter_api/main.go` is wiring only, no business logic:

```
config.Load()
supervision.New(...)          -> go Run(ctx)
daemonclient.New(...)         -> go Run(ctx)
reporting.New(...), scanning.New(...)
go runRearm(ctx, ...)
rest.Router(...), ws.RegisterRoutes(...)
http.Server.ListenAndServe()
<block on SIGINT/SIGTERM>
httpServer.Shutdown() -> <-supervisorDone
```

## Ports this API talks to (all on the daemon side, never exposed to the frontend directly)

| Port | Daemon component | Direction | Scope |
|---|---|---|---|
| **8767** | control channel | bidirectional | one shared connection, redialed on every daemon restart |
| **8766** | scan-result stream | push-only | dialed by `scanning`'s hub on the first active scan, closed on the last stop |
| **9000-9999** | per-device report stream | push-only | dialed by `reporting`'s hub per `START_REPORTING` response, one per active device |

## Build, run, test

See `CLAUDE.md`'s **Commands** section for the exact build/run invocation. Unit tests are
colocated `_test.go` files (`go test ./...`, no build tag — this is the Go-idiomatic
equivalent of the daemon's separate `tests/` tree, since `_test.go` + build tags already give
Go the same separation without a parallel directory). Integration tests live under
`integration_tests/`, build-tagged `integration`, and run via `./run_integration_tests.sh` —
mirroring the daemon's own `run_all_tests.sh`, including the sudo re-exec for the one suite
(`reporting`) that needs a raw GOOSE socket.

## Where to go next

- **Working on a specific feature?** Read its doc in `docs/features/` — overview → public API
  surface → per-file breakdown → threading model → known limitations → cross-feature
  dependencies → tests, same shape as the daemon's own feature docs.
- **Building the frontend** (a separate, context-free Claude session)? Read
  `FRONTEND_API_GUIDE.md` — it's self-contained and assumes no access to this source tree.
- **What are the hard constraints this codebase won't violate?** `CLAUDE.md`'s Hard Rules
  section (never re-implement GOOSE/MMS parsing, treat the daemon's JSON envelopes as an
  external stable contract).
