# station_signal_daemon — Overview

Backend daemon that reports IEC 61850 traffic: it sniffs GOOSE messages off the wire and
subscribes to MMS report control blocks (BRCB/URCB) on substation IEDs, normalizes both into
JSON, and forwards them to a consuming API layer over websockets. This doc is the map — for
current-state facts and hard rules see `CLAUDE.md` at the repo root; for the incident-by-incident
history behind every design decision, see `CHANGELOG.md`; for how to integrate against a running
instance as a black-box network service, see `AGENT_API_GUIDE.md`.

## What it does, end to end

Given an IED's host/port, the daemon:

1. Fetches (or is handed) that IED's SCL (`.icd`/`.cid`/`.scd`) so it knows what to subscribe to
   — never discovers structure over the wire at runtime (see Hard Rules in `CLAUDE.md`).
2. Connects to it over MMS and enables every Report Control Block the SCL declares.
3. Subscribes to every GOOSE Control Block the SCL declares, on a chosen local NIC.
4. Normalizes both MMS reports and GOOSE frames into one common JSON shape and streams only
   **genuine changes** (never a duplicate, never the bootstrap snapshot) out over a per-device
   websocket.

It can do this for several IEDs concurrently, and separately can run background subnet scans to
discover candidate IEDs an operator doesn't already know the address of. Every one of these
actions — start/stop reporting, start/stop scanning — is driven by one always-on control
websocket; there is no other interface (no CLI, no config file, no REST).

## Architecture: feature-first, with three sequencing layers on top

The repo is organized feature-first: `src/features/<name>/` holds one self-contained protocol/
transport concern each, with `service/` (public API), `domain/` (pure logic, no third-party
includes), `data/` (third-party/library/file integration), and sometimes `utils/`. Nine features
exist:

| Feature | Job |
|---|---|
| [`scl_bootstrap`](features/scl_bootstrap.md) | one-shot: probe hosts for MMS, fetch one SCL file over MMS file services |
| [`ied_model`](features/ied_model.md) | parses SCL into the data model everything else reads from |
| [`ied_model_online_loader`](features/ied_model_online_loader.md) | fallback: builds an equivalent model by walking a live device's MMS directory, for devices with no SCL file service |
| [`goose_subscriber`](features/goose_subscriber.md) | subscribes to GOOSE Control Blocks, event-driven raw-socket reception |
| [`mms_report_client`](features/mms_report_client.md) | connects over MMS, enables RCBs, delivers normalized reports |
| [`ipc_dispatcher`](features/ipc_dispatcher.md) | per-device websocket relaying normalized report/GOOSE JSON out |
| [`ied_discovery`](features/ied_discovery.md) | two-stage (TCP probe + real MMS association) verification of candidate hosts on a subnet |
| [`scan_dispatcher`](features/scan_dispatcher.md) | shared websocket relaying "device found" scan events out |
| [`control_dispatcher`](features/control_dispatcher.md) | the one bidirectional websocket — the daemon's only external interface |

Three more directories sit alongside `src/features/` as top-level siblings — each *sequences*
features into a runnable pipeline rather than being a protocol concern itself:

| Layer | Job |
|---|---|
| [`src/orchestration/`](features/orchestration.md) | runs the full pipeline (ipc_dispatcher → scl_bootstrap → ied_model → mms_report_client → goose_subscriber) for **one** IED |
| [`src/scan_orchestration/`](features/scan_orchestration.md) | runs `ied_discovery` + `scan_dispatcher` as a continuous, multi-scan-capable background service |
| [`src/device_manager/`](features/device_manager.md) | runs **several** `orchestration` pipelines concurrently, one per physical IED, each with its own auto-assigned port and `deviceId` |

`control_dispatcher` is the top of this stack: it receives commands and relays them into
`device_manager` (`START_REPORTING`/`STOP_REPORTING`) and `scan_orchestration`
(`START_SCAN`/`STOP_SCAN`).

## `main.c`'s boot sequence

`src/main.c` (141 lines) is wiring only, no business logic. It takes **no arguments** and has no
terminal/CLI surface:

```
DeviceManager_create()
ScanOrchestration_create()  -> ScanOrchestration_setDeviceFoundCallback (printf passthrough)
ControlDispatcher_create(deviceManager, scanOrchestration)
ControlDispatcher_start()   -> binds ws://127.0.0.1:8767
<block on SIGINT/SIGTERM>
ControlDispatcher_destroy() -> ScanOrchestration_destroy() -> DeviceManager_destroy()
```

Teardown is the reverse of startup: stop accepting new commands first, then stop any active
scans, then drain and tear down every still-running device. There is no boot-time device and no
interactive prompt — every device/scan lifecycle action goes exclusively through
`control_dispatcher`'s four JSON commands (see `AGENT_API_GUIDE.md` for the full wire contract).

## Ports

| Port | Owner | Direction | Scope |
|---|---|---|---|
| **8767** | `control_dispatcher` | bidirectional | one shared instance, always on |
| **8766** | `scan_dispatcher` | push-only | one shared instance, refcounted 0→1 on first active scan, 1→0 on last stop |
| **9000-9999** (default range) | `ipc_dispatcher` | push-only | one port per running device, auto-assigned by `device_manager`, freed on `STOP_REPORTING` |

## Build, run, test

See `CLAUDE.md`'s **Commands** section for the exact manual build invocation (no CMakeLists.txt
or root Makefile exists yet — a deliberate, acknowledged gap, not an oversight), the unit-test
runner (`cd tests && make run`), the per-feature E2E suites (`cd integration_tests/<feature> &&
make run`, several needing `sudo` for raw-socket GOOSE access), and `./run_all_tests.sh` which
runs everything in one pass.

## Where to go next

- **Working on a specific feature/layer?** Read its doc in `docs/features/` — every one follows
  the same shape: overview → public API surface → per-file breakdown → threading model → known
  limitations → cross-feature dependencies → tests. They go deeper than this doc or `CLAUDE.md`
  on purpose — file-by-file, function-by-function.
- **Integrating against a running daemon as a service** (a different Claude conversation, an
  external API layer, a frontend)? Read `AGENT_API_GUIDE.md` — it's self-contained and assumes
  no access to this source tree.
- **Why is something built the way it is, and what changed over time?** `CHANGELOG.md` has the
  full incident-by-incident history (real-hardware bugs, root causes, reversed decisions) behind
  every current-state fact in `CLAUDE.md` and in these feature docs.
- **What are the hard constraints this codebase won't violate?** `CLAUDE.md`'s Hard Rules section
  (libiec61850-only protocol handling, no cyclic polling except one narrow GOOSE-liveness
  exception, no over-the-wire tree discovery except one narrow online-loader fallback, no
  dangling connections, don't touch `third_party/`).
