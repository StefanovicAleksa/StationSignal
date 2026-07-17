# ied_reporter_daemon

Backend daemon that reports IEC 61850 traffic: it sniffs GOOSE messages off the wire and
subscribes to MMS report control blocks (BRCB/URCB) on substation IEDs, normalizes both into
JSON, and streams them out over websockets to a consuming API layer.

It's a background process with no CLI and no config file — everything is driven by one always-on
control websocket. Given an IED's host, it fetches (or is handed) its SCL file, connects over
MMS, enables its Report Control Blocks, subscribes to its GOOSE Control Blocks, and streams
**only genuine changes** out over a per-device websocket. It can run several IEDs concurrently,
and separately can run background subnet scans to discover IEDs on the network.

## Status

Functional, actively developed, not packaged. Protocol handling is entirely via
[libiec61850](https://libiec61850.com/) — this codebase never hand-rolls GOOSE or MMS parsing.
There's no build system yet beyond a manual `gcc` invocation (see Building below); that's a known
gap, not an oversight.

## Documentation

| Doc | For |
|---|---|
| [`docs/DAEMON_OVERVIEW.md`](docs/DAEMON_OVERVIEW.md) | Architecture map: how the pieces fit together, boot sequence, ports, where to go next |
| [`docs/AGENT_API_GUIDE.md`](docs/AGENT_API_GUIDE.md) | Self-contained protocol reference for integrating against a running daemon over websockets (control channel, report streams, worked JSON flows) |
| [`docs/features/`](docs/features) | In-depth internal doc per feature/layer — overview, public API, file-by-file breakdown, threading model, tests |
| [`CLAUDE.md`](CLAUDE.md) | Compact current-state reference (build commands, hard rules, testing conventions) |
| [`CHANGELOG.md`](CHANGELOG.md) | Incident-by-incident history behind every design decision |

## Architecture at a glance

Feature-first: `src/features/<name>/` holds one self-contained protocol/transport concern each
(`scl_bootstrap`, `ied_model`, `ied_model_online_loader`, `goose_subscriber`,
`mms_report_client`, `ipc_dispatcher`, `ied_discovery`, `scan_dispatcher`, `control_dispatcher`).
Three top-level layers sequence them into runnable pipelines: `src/orchestration/` (one IED),
`src/scan_orchestration/` (continuous background subnet scanning), `src/device_manager/`
(several IEDs concurrently). `control_dispatcher` is the daemon's only external interface — one
bidirectional websocket receiving `START_REPORTING`/`STOP_REPORTING`/`START_SCAN`/`STOP_SCAN`
commands and relaying them into `device_manager`/`scan_orchestration`.

Full detail: [`docs/DAEMON_OVERVIEW.md`](docs/DAEMON_OVERVIEW.md).

## Building

No `CMakeLists.txt` or root Makefile exists yet. `src/main.c` builds manually, compiled together
with every `.c` file under `src/orchestration/`, `src/scan_orchestration/`,
`src/device_manager/`, and all `src/features/<feature>/` directories:

```sh
gcc -g -Wall -Isrc -idirafter third_party/include src/main.c \
    src/orchestration/*/*.c src/scan_orchestration/*/*.c src/device_manager/*/*.c \
    src/features/*/*/*.c -o /tmp/ied_reporter_daemon \
    -Lthird_party/lib -liec61850 -lhal -lmxml -lwebsockets -lcjson -lpthread \
    && /tmp/ied_reporter_daemon
```

Also wrapped by `rebuild_proj.sh`. `sudo` is only required once a client asks the daemon to
report on a device over GOOSE (raw socket) — the process itself starts and idles fine without it.

## Running

`main.c` takes no arguments. It starts one always-on control websocket at
`ws://127.0.0.1:8767` and blocks until `SIGINT`/`SIGTERM`. Every device/scan lifecycle action —
starting/stopping reporting on an IED, starting/stopping a subnet scan — goes through that one
websocket's four JSON commands. See [`docs/AGENT_API_GUIDE.md`](docs/AGENT_API_GUIDE.md) for the
full wire protocol and worked examples.

## Testing

```sh
cd tests && make run                              # unit tests, hermetic
cd integration_tests/<feature> && make run         # per-feature E2E, real ied_simulator
./run_all_tests.sh                                  # everything in one pass (re-execs under sudo)
```

Several E2E suites need `sudo` for raw-socket GOOSE access (`goose_subscriber`, `orchestration`,
`device_manager`, part of `control_dispatcher`); every other suite is plain TCP/loopback MMS.
Full breakdown per feature in each `docs/features/*.md`'s own Tests section.

## Third-party

Vendored under `third_party/` — don't hand-edit. Primary dependency is
[libiec61850](https://libiec61850.com/); also `mxml` (SCL parsing), `libwebsockets` (transport),
`cJSON` (control-plane JSON).
