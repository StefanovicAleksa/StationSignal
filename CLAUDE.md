# IEC 61850 GOOSE/MMS Reporter — ied_reporter_daemon

## Purpose
Backend daemon that reports IEC 61850 traffic: sniffs GOOSE messages off the wire and
subscribes to MMS report control blocks (BRCB), normalizes both into JSON, and forwards
them to the consuming API layer. This file governs this repo (root = the daemon itself).

## Commands
- Build daemon: **TODO — no CMakeLists.txt or root Makefile exists yet.** `src/main.c` can be
  built manually (same throwaway-linkage-probe convention as the smoke tests below) by
  compiling it together with every `.c` file under `src/orchestration/`,
  `src/scan_orchestration/`, and `src/device_manager/`, and all nine `src/features/<feature>/`
  directories (`service`/`data`/`domain`/`utils`), e.g.:
  `gcc -g -Wall -Isrc -idirafter third_party/include src/main.c
  src/orchestration/*/*.c src/scan_orchestration/*/*.c src/device_manager/*/*.c
  src/features/*/*/*.c -o /tmp/ied_reporter_daemon -Lthird_party/lib -liec61850 -lhal -lmxml
  -lwebsockets -lcjson -lpthread && /tmp/ied_reporter_daemon` (also wrapped by
  `rebuild_proj.sh`) — this is a manual stopgap, not a substitute for a real build system; don't
  invent or guess a permanent build command, ask before assuming one. `sudo` is only required if
  a real device is actually reached over GOOSE (raw socket) once a client asks the daemon to
  report on one — the process itself starts and idles fine without it.
  - **`main.c` takes no arguments and has no terminal/CLI surface at all** — it is a pure
    background process-runner for an external API layer. It creates `device_manager` +
    `scan_orchestration` + `control_dispatcher`, starts the one always-on control websocket
    (default `127.0.0.1:8767`), and blocks until `SIGINT`/`SIGTERM`. Every device/scan lifecycle
    action is driven exclusively through that one control websocket's four JSON commands — there
    is no boot-time device and no interactive discovery prompt anymore (both were removed once
    `control_dispatcher` grew a full command set covering the same ground). See
    `control_dispatcher/`'s own Architecture bullet below for the full envelope shape.
  - `START_REPORTING {host, mmsPort, iedName?, interfaceId, sclFilePath?, acseAuthPassword?,
    accessMode?}` → `{deviceId, wsPort}`: starts one IED's full MMS+GOOSE reporting pipeline via
    `DeviceManager_startReporting`, auto-assigning that device its own `ipc_dispatcher` websocket
    port from `device_manager`'s configured range (default 9000-9999), returned as `wsPort`.
    `iedName` omitted/empty auto-detects — works only if the SCL declares exactly one `<IED>`
    (see `orchestration/`'s own bullet) — UNLESS `sclFilePath` is also given, in which case
    `device_manager` requires `iedName` explicitly (see its own bullet below) — a stricter
    contract than `Orchestration_runFromLocalFile`'s own auto-detect, deliberately, per this
    feature's own explicit requirement. `sclFilePath` omitted (the common case): loads the
    device's structure via `scl_bootstrap` (network SCL fetch) and, if that comes back with
    exactly `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` (a real, connectable MMS/GOOSE device
    that never serves an SCL file over file services — confirmed in practice against OMICRON IED
    Scout's "Simulate IED" mode), `device_manager` automatically retries once via
    `Orchestration_runFromOnlineDiscovery` (via `DeviceManagerBootstrapPolicy_run`) — builds the
    model directly from the live device's own MMS data model instead. See
    `ied_model_online_loader/`'s own Architecture bullet below and the "No over-the-wire tree
    discovery" Hard Rule's documented exception. `sclFilePath` given: skips `scl_bootstrap`
    entirely and loads this local SCL file instead (`Orchestration_runFromLocalFile`) — for
    devices whose MMS server doesn't implement file services at all. See `orchestration/`'s own
    bullet for the stage-by-stage behavior and `device_manager/`'s own bullet for the full
    reserve/bootstrap/finalize sequencing.
  - `STOP_REPORTING {deviceId}` → `{deviceId}`: stops and tears down the device started by the
    `START_REPORTING` call that returned this `deviceId`, freeing its `ipc_dispatcher` port for
    reuse.
  - `START_SCAN {interfaceId, mmsPort, sweepIntervalMs?}` → `{scanId}`: starts a continuous
    background subnet scan (`scan_orchestration`) on `interfaceId`/`mmsPort`, streaming
    discovered IEC 61850 MMS devices over the shared `scan_dispatcher` websocket (default port
    8766 — one shared instance for every concurrent scan in the process, not one per scan; see
    `scan_orchestration/`'s own bullet below). Starts that shared websocket on the first
    concurrently-active scan (0→1) if not already running.
  - `STOP_SCAN {scanId}` → `{scanId}`: stops the scan started by the `START_SCAN` call that
    returned this `scanId`. Tears down the shared `scan_dispatcher` websocket if this was the
    last active scan (1→0).
- Build + run IED simulator (integration test fixture): `cd integration_tests/ied_simulator && make`
- Generate simulator model: `./integration_tests/ied_simulator/scripts/generate_model.sh`
- Run unit tests: `cd tests && make run` — strictly unit, no file I/O beyond two
  self-contained temp-file cases (`ied_model`, `orchestration`), fast. `tests/Makefile` does
  **not** auto-discover — it's an explicit `TESTS` list plus one hand-written build rule per
  binary; adding a new feature's unit tests means editing this Makefile, not just dropping
  files in a new `tests/<feature>/` directory.
- Run the `ied_model` E2E test: `cd integration_tests/ied_model && make run` — loads a
  real fixture (`fixtures/breaker1.cid`) through the real service API.
- Run the `scl_bootstrap` E2E test: `cd integration_tests/scl_bootstrap && make run` — runs a
  real `ied_simulator` "Reporter1" IED in-process with its MMS file services pointed at a real
  fixture directory, TCP-probes and fetches its SCL file over a real MMS association (plus
  ACSE-password-auth-retry and negative-path cases). No `sudo` needed — plain TCP/MMS only.
- Run the `ied_model_online_loader` E2E test: `cd integration_tests/ied_model_online_loader &&
  make run` — runs a real `ied_simulator` "Reporter1" IED in-process with its MMS file services
  pointed at a real, empty fixture directory (no SCL file at all — reproduces the exact real-world
  precondition, `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND`), then drives `IedModelOnlineLoader_build`
  directly against it and asserts the discovered report/GOOSE targets and dataset members match
  `sim_server.c`'s own hand-built shape exactly. No `sudo` needed — plain TCP/MMS only.
- Run the `mms_report_client` E2E test: `cd integration_tests/mms_report_client && make run`
  — runs a real `ied_simulator` "Reporter1" IED in-process, connects the real service, flips
  a value server-side, asserts a real report arrives. Also proves ACSE password auth
  end-to-end against a real `SimServer_requireAuthentication`-protected instance: a correct
  password connects and enables the RCB, a wrong one never does.
- Run the `goose_subscriber` E2E test: `cd integration_tests/goose_subscriber && sudo make run`
  — runs the same "Reporter1" IED in-process publishing real GOOSE on `lo`, connects the real
  service, flips a value server-side, asserts a real GOOSE-carried record and a `VALID`
  liveness transition both arrive. **Needs `sudo`** — the only test in this repo that opens a
  raw Ethernet socket (every other test/E2E path is plain TCP/loopback MMS, no elevated
  privilege needed).
- Run the `orchestration` E2E test: `cd integration_tests/orchestration && sudo make run` —
  runs the same "Reporter1" IED in-process (MMS file services + buffered reports + GOOSE all
  at once) and drives the real, full `ipc_dispatcher -> scl_bootstrap -> ied_model ->
  mms_report_client -> goose_subscriber` sequence against it over loopback, connecting a real
  hand-rolled websocket client to orchestration's own `ipc_dispatcher` port and asserting both a
  real MMS-report JSON message and a real GOOSE JSON message arrive over it. **Needs `sudo`** —
  inherits the GOOSE-subscriber step's raw-socket requirement. A second test in the same binary,
  `test_onlineDiscoveryFallback_afterNoSclFileFound_endToEnd`, proves the online-discovery
  fallback end to end against a second simulator instance whose MMS file services point at a
  real, empty `fixtures/no_scl_files/` directory: asserts `Orchestration_run` genuinely fails
  with `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND`, then `Orchestration_runFromOnlineDiscovery`
  against the same host succeeds and real report/GOOSE JSON still arrives — the expected
  reporting RCB differs from the first test (`urcbDyn`, not `brcbMain`) since `brcbMain`/
  `brcbDup`/`rcbMulti01` are parented under `LLN0`, which has no `FC=ST/MX` attributes of its own
  in this simulator, so `mms_report_client`'s existing dynamic-dataset fallback can't synthesize
  a dataset for them from an online-discovered (`datasetReference=NULL`) RCB — confirmed
  empirically (log output shows exactly these three failing setRCBValues while `urcbDyn`
  succeeds), not a defect in discovery itself.
- Run the `ipc_dispatcher` E2E test: `cd integration_tests/ipc_dispatcher && make run` —
  starts a real `IpcDispatcher` directly (real bind, real libwebsockets service thread, not
  through orchestration), connects a hand-rolled minimal websocket test client, drives hand-built
  `MmsReportRecord`/`GooseSubscriberRecord` fixtures through `IpcDispatcher_onMmsReport`/
  `_onGooseRecord`, and asserts real JSON arrives over the real socket. No `sudo` needed
  (loopback TCP only, same as `mms_report_client`) and no `ied_simulator` needed — unlike the
  other E2E tests, this feature has no external IED to talk to, only its own transport.
- Run the `ied_discovery` E2E test: `cd integration_tests/ied_discovery && make run` — runs a
  real `ied_simulator` "Reporter1" IED in-process, drives `IedDiscovery_verifyHost` against it
  over loopback (real TCP probe + real MMS/ACSE association, no browse/fetch), and proves the
  ACSE-auth-retry symmetry the same way `scl_bootstrap`'s own E2E test does. No `sudo` needed.
  Deliberately does **not** exercise `IedDiscovery_scanSubnet`/`getifaddrs` — a real LAN subnet
  scan is machine-topology-dependent, not something this test can assert on; the one
  deterministic `getifaddrs` case (the `maxHosts` ceiling against loopback's own real netmask)
  lives in `tests/ied_discovery/test_ied_discovery_api.c` instead. Manually verify a real subnet
  scan by just running the daemon with no `host` argv on a machine with a real neighbor IED —
  same "manual smoke test, not automatable" class as the GOOSE loopback smoke test below.
- Run the `scan_dispatcher` E2E test: `cd integration_tests/scan_dispatcher && make run` —
  starts a real `ScanDispatcher` directly (real bind, real libwebsockets service thread, not
  through `scan_orchestration`), connects a hand-rolled minimal websocket test client, drives
  hand-built `ScanDispatcher_publishDeviceFound` calls, and asserts real `SCAN_RESULT` JSON
  arrives over the real socket, in order. No `sudo` needed (loopback TCP only, same as
  `ipc_dispatcher`) and no `ied_simulator` needed — this feature has no external IED to talk to,
  only its own transport.
- Run the `scan_orchestration` E2E test: `cd integration_tests/scan_orchestration && make run` —
  drives the real `ScanOrchestration_startScan`/`_stopScan` against the real `lo` interface with
  two different `mmsPort`s, proving the refcounted shared-websocket sequencing end to end (a 2nd
  concurrent scan shares the already-bound dispatcher, stopping one of two leaves it running,
  stopping the last one tears it down, a subsequent scan cleanly rebinds the same port) via the
  same hand-rolled websocket test-client shape as `scan_dispatcher`'s own E2E test. Sweeps
  against `lo` are *expected* to fail with `IED_DISCOVERY_ERR_SUBNET_TOO_LARGE` (loopback's real
  netmask is `/8`, deterministically exceeding the default `maxHosts` ceiling — see
  `tests/ied_discovery/test_ied_discovery_api.c`'s own proof of this) and are tolerated
  gracefully; this test proves sequencing/refcounting/threading, not sweep success — same
  reasoning `integration_tests/ied_discovery/` already documents for avoiding a real `scanSubnet`
  call over a real interface. No `sudo` needed.
- Run the `device_manager` E2E test: `cd integration_tests/device_manager && sudo make run` —
  drives two real, concurrent `DeviceManager_startReporting` calls (from two threads) against
  two real `ied_simulator` "Reporter1" instances at two different `mmsPort`s, proving: the
  two-phase-locked registry doesn't serialize one device's slow bootstrap+MMS+GOOSE sequence
  behind the other's (a coarse, deliberately non-flaky wall-clock bound — the precise "lock isn't
  held across the slow call" property is proven structurally by
  `tests/device_manager/test_device_manager_registry.c` instead); each gets a distinct deviceId
  and a distinct, real, independently-connectable `ipc_dispatcher` websocket streaming real GOOSE
  JSON; stopping one leaves the other running; stopping the second frees its port for reuse by a
  subsequent start. **Needs `sudo`** — inherits the GOOSE-subscriber step's raw-socket requirement
  via every `DeviceManager_startReporting` call.
- Run the `control_dispatcher` E2E test: `cd integration_tests/control_dispatcher && sudo make
  run` — a hand-rolled RFC6455 client that both SENDS masked command frames (the first E2E test
  in this repo to send a client→server frame — every other dispatcher is push-only) and receives
  responses. Malformed-JSON and unknown-action cases run with no privilege and no simulator at
  all (fail entirely on the lws thread before ever reaching `device_manager`); a third case
  drives a real `START_REPORTING`/`STOP_REPORTING` round trip against a real `ied_simulator`
  instance, asserting the returned `wsPort` streams real GOOSE JSON (proving the whole chain:
  control message → `device_manager` → `orchestration` → per-device `ipc_dispatcher`) and that
  the port is torn down after stop. **Needs `sudo`** for that third case only, same reasoning as
  `device_manager`'s own E2E test.
- Raw-socket loopback smoke test (build manually, no Makefile — throwaway linkage/behavior
  probe): `gcc -g -Wall -Isrc -idirafter third_party/include tools/smoke_tests/goose_loopback_smoke_test.c
  -o /tmp/goose_loopback_smoke_test -Lthird_party/lib -liec61850 -lhal -lpthread && sudo
  /tmp/goose_loopback_smoke_test` — proves a bare `GoosePublisher`/`GooseReceiver` pair
  round-trips a real GOOSE frame over `lo` before trusting that assumption in the E2E test above.
- Run via `sudo` once built — raw socket access required for GOOSE sniffing.
- `./run_all_tests.sh` runs every suite above in one pass: the unit tests (`tests/`) followed by
  every `integration_tests/<feature>/` E2E suite, in the same `make clean && make run` shape each
  suite's own bullet already documents. Re-execs itself under `sudo` if not already root (some
  suites need `CAP_NET_RAW` for GOOSE, most don't — running everything under one `sudo` umbrella
  is simpler than tracking which is which per invocation). Reports pass/fail per suite AND
  aggregates every individual Unity `N Tests M Failures P Ignored` summary line across the whole
  run (`tests/` alone runs many separate Unity binaries, each with its own such line) into one
  grand total printed at the end. **This suite list is hand-maintained inside the script, not
  auto-discovered — whenever a test suite is added or removed (a new `tests/<feature>/` Makefile
  entry in `tests/Makefile`'s own `TESTS` list, or a new/removed `integration_tests/<feature>/`
  directory), update `run_all_tests.sh`'s `run_suite` calls in the same change.**

## Current State (update as this evolves)
- `src/main.c` now wires the real thing: `OrchestrationConfig_defaults` (which also defaults
  `config.ipcDispatcherConfig`, overridable via an optional 5th argv slot for the websocket
  port) -> `Orchestration_create` -> registers the three remaining `printf`-based
  connection-state/RCB-status/liveness-diagnostic passthroughs (report/GOOSE DATA records have
  no caller-facing setter at all anymore - see `ipc_dispatcher`'s own bullet below for why) ->
  (if `host` argv was omitted/empty: `ied_discovery`'s interactive scan/manual-add/pick flow via
  `src/main_discovery_prompt.c`, see that feature's own bullet below) -> `Orchestration_run`
  (port/interface from argv; host either argv-supplied or the just-picked one; IED name either
  argv-supplied or empty, triggering `orchestration`'s own auto-detect) -> blocks on
  `SIGINT`/`SIGTERM` ->
  `Orchestration_destroy`. Notably, `main.c` never includes
  `features/ipc_dispatcher/service/ipc_dispatcher_api.h` at all - orchestration owns that
  feature's entire lifecycle end-to-end (see below), the same way it already owns ied_model/
  mms_report_client/goose_subscriber's. All six `src/features/` (`scl_bootstrap/`,
  `ied_model/`, `mms_report_client/`, `goose_subscriber/`, `ipc_dispatcher/`, `ied_discovery/`)
  plus `src/orchestration/` are now implemented (see Architecture below) — every feature named in
  the Expected-features list exists, `ied_discovery` being a later, deliberate, user-requested
  addition beyond the original five (see its own bullet for why it's not "inventing an unrelated
  feature").
- **`main.c`'s `host`/`iedName` argv no longer default to `"127.0.0.1"`/`"Reporter1"`** — those
  only ever matched the bundled `integration_tests/ied_simulator` fixture and directly
  contradicted the point of `ied_discovery` (an operator who doesn't already know the IP
  shouldn't need to already know the IED name either). Omitting `host` now triggers the
  interactive discovery flow instead of silently defaulting to the test simulator's address; no
  automated test depended on the old defaults (every `tests/`/`integration_tests/` case calls
  `orchestration`'s API directly, never the compiled `main.c` binary).
- **`main.c`'s `host`-omitted flow was rewired from a one-shot blocking scan to a continuous
  background scan (`scan_orchestration`)**, so `main.c` can prove the new process actually works
  end-to-end the same way it already proves orchestration's report/GOOSE pipeline works — this
  mirrors the daemon's broader direction of becoming a set of background processes eventually
  managed by an external Go API + local-network frontend (not built yet), rather than a
  one-shot CLI tool. Previously `main_discovery_prompt.c` called `IedDiscovery_scanSubnet` once,
  blocking until the whole sweep finished, then presented a static list. Now `main.c` starts a
  `ScanOrchestration` scan (interactive-flow default, on `interface`), registers a printf
  device-found callback (same style as the existing `onReportConnState`/`onRcbStatus`/
  `onGooseStatus` passthroughs) so discoveries print as they stream in, and
  `main_discovery_prompt.c`'s loop now re-snapshots the scan's live, growing host list
  (`ScanOrchestration_snapshotDiscoveredHosts`) fresh before every prompt reprint instead of
  reading a static list — the scan keeps running concurrently in the background the whole time
  the operator is at the prompt. The manually-typed-IP path is unchanged (still a separate,
  plain `IedDiscoveryHandle` calling `IedDiscovery_verifyHost` directly — `ied_discovery` itself
  was not modified at all by this change). Once a device is picked (from the list or manually
  verified), `main.c` stops that scan (`ScanOrchestration_stopScan` — may block, see
  `scan_orchestration/`'s own bullet's documented limitation; `main.c` prints a "Stopping
  scan..." diagnostic first) before falling through to the existing, completely unmodified
  `scl_bootstrap`/`orchestration` pipeline. See `scan_dispatcher/`'s and `scan_orchestration/`'s
  own Architecture bullets below for the full mechanism.
- **`main.c` was rewritten again, most recently, to support reporting on MULTIPLE IEDs at once**
  (explicit user request) — its primary long-lived job is now `device_manager` +
  `control_dispatcher` (see their own Architecture bullets below), not one fixed boot-time device.
  The entire inline `if (sclFilePath) {...} else {...}` bootstrap-fallback block plus the direct
  `Orchestration_create`/`Orchestration_set*Callback` calls this bullet originally described above
  are GONE from `main.c` — extracted verbatim into `device_manager/domain/
  device_manager_bootstrap_policy.c` so both `main.c`'s own boot-time device and
  `control_dispatcher`'s worker thread share exactly one copy. **Consequence, an accepted
  simplification, not a silent regression**: the three diagnostic `printf` passthroughs
  (`onReportConnState`/`onRcbStatus`/`onGooseStatus`) are gone too — `device_manager` creates each
  device's own `OrchestrationHandle` internally and never exposes it to `main.c`, so there's no
  attachment point for them anymore; the control websocket is the real diagnostic/status
  interface going forward, not process-local `printf`. **Breaking argv change**: the old 5th argv
  slot (an `ipc_dispatcher` port override) is gone — there's no longer one fixed dispatcher to
  point an override at, since `device_manager` owns per-device port allocation from its own
  configured range; `acseAuthPassword`/`sclFilePath` each moved one slot earlier as a result (see
  this file's own `main.c` Commands bullet above for the exact new layout). The `host`-omitted
  scan flow (previous bullet, above) is otherwise unchanged in spirit — once it picks a host, that
  host is now handed to the same `DeviceManager_startReporting` call the argv-supplied-host path
  uses, instead of calling `Orchestration_run`/`_runFromLocalFile` directly the way this file used
  to — one path for "start a device" instead of two divergent ones. A failed boot-time device (scan
  picked nothing, or `DeviceManager_startReporting` itself failed) is no longer fatal to the whole
  process either — `control_dispatcher` stays up regardless, since it's this daemon's real
  interface now. `rebuild_proj.sh` and this file's own manual build command were updated to
  include `src/device_manager/*/*.c` alongside `src/orchestration/`/`src/scan_orchestration/`'s
  own `.c` files (the latter two were themselves missing from `rebuild_proj.sh` until this pass —
  a gap from `scan_orchestration`'s own original commit, fixed here since `main.c` cannot build
  without them regardless).
- **`main.c` was rewritten once more, most recently, to remove its remaining terminal/CLI surface
  entirely** (explicit user request, turning the daemon into a pure background process-runner for
  an external API layer) — **every bullet above describing argv slots, a boot-time device, or an
  interactive discovery prompt is now historical only and no longer reflects current behavior.**
  `main.c` now takes **zero arguments** (`int main(void)`) and does nothing but create
  `device_manager` + `scan_orchestration` + `control_dispatcher`, start the one always-on control
  websocket, and block on `SIGINT`/`SIGTERM` until torn down in the same reverse order as before.
  The entire `host`-omitted background-scan-plus-interactive-prompt block and the
  argv-supplied-host boot-time `DeviceManager_startReporting` call (both described above) are
  gone, along with `src/main_discovery_prompt.c`/`.h` (deleted — nothing else referenced them).
  `control_dispatcher`'s `START_SCAN`/`STOP_SCAN` and `START_REPORTING`/`STOP_REPORTING` commands
  (see its own Architecture bullet below, and this file's own `main.c` Commands bullet above for
  the full request/response schema of all four) are now the daemon's **only** way to start/stop
  anything — this was already fully implemented and tested (the scan actions landed in an earlier
  pass this doc had failed to document at all until now, a real doc/code drift caught while
  planning this change). `rebuild_proj.sh` and this file's own manual build command were updated
  to drop `src/main_discovery_prompt.c`. `ied_discovery`'s own service header doc comment was
  updated to stop referencing `main_discovery_prompt.c` as the discovery interaction medium.
- **Bugfix surfaced by wiring `ipc_dispatcher` into orchestration's own rollback paths**:
  `MmsReportClientConnection_destroy` (`src/features/mms_report_client/data/mms_report_client_connection.c`)
  used to destroy `handle->wakeSignal` (the semaphore) *before* `IedConnection_destroy` - but
  `IedConnection_destroy` can synchronously re-fire `onStateChanged` (it internally closes the
  connection again even if `MmsReportClientConnection_stop` already closed it), which
  unconditionally posts that semaphore. This use-after-free crashed intermittently whenever a
  caller destroyed an actively-connecting client - previously unreachable in practice because
  the only code path that destroys an already-started `MmsReportClientHandle` mid-connection is
  orchestration's own "a later stage failed, roll back the already-started report client"
  rollback branch, and every prior orchestration E2E run happened to have GOOSE succeed (via
  `sudo`), so that branch was never actually exercised until orchestration started deterministically
  reaching it in an environment without `CAP_NET_RAW`. Fixed by destroying the connection before
  the semaphore (reverse of the old order) - see that function's own comment for the full story.
- **Second bugfix in the same rollback family, surfaced by real-hardware testing against a ~40-RCB
  device**: `enableAllTargets`/`enableOneTarget` (same file as above) used to loop through every
  cached RCB target with no check for a concurrent stop request. When orchestration's fail-hard
  rollback calls `MmsReportClient_destroy` on an already-started client (e.g. because GOOSE
  subscriber start failed on a bad/absent network interface) while the supervisor thread is still
  mid-loop enabling later RCBs on a separate thread, `MmsReportClientConnection_stop`'s
  `IedConnection_close` races that in-flight loop with no coordination - the RCB being processed
  at that exact moment fails (a timeout, since its connection is being pulled out from under it -
  inherent and accepted, not fixable without deeper library-level synchronization), and *every*
  remaining target then also gets attempted and fails immediately with `IED_ERROR_CONNECTION_LOST`
  - a long, noisy, entirely wasted cascade (confirmed directly: one interface failure turned into
  ~40 doomed MMS round-trips and ~40 spurious error lines). Previously unreachable in practice
  for the same reason as the bugfix above - every fixture/E2E test has 2-4 RCBs on loopback,
  finishing the whole enable loop in well under a millisecond, nowhere near enough of a window for
  a concurrent rollback to land mid-loop. Fixed by checking `handle->stopRequested` once per
  `enableAllTargets` loop iteration and again at the top of `enableOneTarget` (defense-in-depth for
  the narrow gap between the loop's own check and the call actually landing) - turns the cascade
  into one expected failure plus a prompt, quiet stop. Also relevant if this same rollback fires
  when `Orchestration_runFromLocalFile`/`_runFromOnlineDiscovery`'s own GOOSE stage fails after
  `mms_report_client` has already started - identical race, same fix, since all three entry points
  share this one `runFromIedModelHandle` tail (see `orchestration/`'s own bullet below).
- `mms_report_client` now supports ACSE password authentication (`MmsReportClientConfig.acseAuthPassword`,
  new `data/mms_report_client_auth.c`) — previously only `scl_bootstrap`'s SCL-discovery
  connection could authenticate, so a real IED requiring auth on every association would let
  the daemon discover+fetch its SCL but then fail outright to establish the actual reporting
  connection. `src/main.c` exposes this as an optional 6th argv slot, reused for both
  `config.bootstrapConfig.acseAuthPassword` and `config.reportClientConfig.acseAuthPassword`
  (same physical IED, same credential, two independent `IedConnection`s). See
  `mms_report_client/`'s own Architecture bullet below for how this differs from
  `scl_bootstrap`'s retry-on-rejection approach.
- **Both reporting workers are now filtered to be strictly event-driven, closing a gap where
  periodic/non-event wire traffic could otherwise reach `ipc_dispatcher` looking like a real
  event.** `mms_report_client` never writes `TrgOps`/`IntgPd` when enabling an RCB (unchanged,
  deliberate — see that feature's own bullet below), so a real IED whose SCL configures
  `period="true"` (integrity triggering — common in real vendor exports, confirmed against this
  repo's own fixtures) will genuinely push periodic integrity reports forever; the one-shot
  `GI` snapshot on enable is also not a "something changed" event. `mms_report_client` filters
  via a per-position value-diff cache (`domain/mms_report_client_usecases.c`'s `buildEntries`,
  through `shouldForwardAndUpdateCache`, backed by
  `MmsReportClientMemberRefCacheEntry.lastForwardedValues`, built once per RCB alongside the
  Gap 4 cache): forwarded only if its value differs from the last one actually forwarded for that
  exact wire position. **`shouldForwardAndUpdateCache` ALWAYS runs this diff-check, for every
  entry, regardless of the server's own `ClientReport_getReasonForInclusion` bitmask — reason is
  never trusted to bypass it.** This wasn't always true: an earlier version of this function
  trusted a real-change bit (`DATA_CHANGE`/`QUALITY_CHANGE`/`DATA_UPDATE`,
  `MmsReportClientUseCases_hasRealChangeReason`) as an unconditional "skip the diff-check, always
  forward" signal — real-hardware testing against a live IED proved that trust unsafe: the device
  tagged hundreds of consecutive, byte-identical reports as `DATA_CHANGE` even though the value
  never actually changed (confirmed directly via `previousValue == value` on every one of them —
  see "IPC / Reporting Out" below for that field). Since `GI` and `DATA_CHANGE` are independent,
  combinable `ReasonForInclusion` bits (confirmed against `third_party/include/iec61850_client.h`
  — nothing stops a server from setting both at once), this also explains "GI reports reaching the
  frontend": a GI-triggered entry that also happens to carry a real-change bit used to bypass
  bootstrap-suppression the same way. `MmsReportClientUseCases_hasRealChangeReason` was deleted
  entirely (no longer called anywhere) once this landed — `reason` is still carried on
  `MmsReportEntry` as informational metadata, just never consulted for the forward/drop decision.
  `goose_subscriber`'s equivalent function was never affected by this bug (GOOSE has no
  `ReasonForInclusion` concept at all and has always diff-checked unconditionally) — that
  asymmetry (GOOSE working correctly while MMS flooded) is what first narrowed the bug down to
  this one function.
  The "nothing cached yet" case (`cached == NULL`) is a **bootstrap** event —
  `shouldForwardAndUpdateCache` silently seeds the cache from it but returns `false` (never
  forwarded). This is now **unconditional**, for the same reason the reason-trust bypass above had
  to go entirely, not just partially: at the time this fix landed, a reconnect reset this same
  cache to `NULL` on every (re-)enable (`MmsReportClientUseCases_resetValueDiffCache`, called from
  `enableOneTarget` — **this reset mechanism was later removed entirely, see the "cache is now
  never reset" redesign further below** — the cache is populated once and preserved forever, and
  `cached == NULL` is now only expected on a position's genuine first-ever observation), so
  "first observation, cached == NULL, tagged with a real-change reason" was *structurally
  indistinguishable* from "a reconnect's redelivered-but-unchanged report, cache freshly reset,
  spuriously tagged with a real-change reason" — the exact pattern the real device demonstrated.
  Trusting reason on the `cached == NULL` branch for one would necessarily also trust it for the
  other, reopening the same bug — this reasoning is why `reason` stays untrusted unconditionally
  even after the reset mechanism itself was later removed. **`mms_report_client` no longer requests GI at all, on any
  enable** (`MmsReportClientConfig.generalInterrogationOnEnable` was removed entirely, along
  with the `ClientReportControlBlock_setGI`/`RCB_ELEMENT_GI` branch in `enableOneTarget` —
  see this feature's own Architecture bullet below for the full reasoning) — GI proved
  unreliable on real hardware (see above), and this makes `mms_report_client` structurally
  identical to `goose_subscriber` in this respect: no artificial snapshot is ever requested at
  all, matching GOOSE's own GI-less design exactly (a foreign client's own GI, or a buffered
  RCB's redelivery on re-enable, can still produce a report this client observes with
  `reason=GI` or a stale-looking `DATA_CHANGE` — those are still handled correctly, just never
  requested by us). **Accepted consequence, previously observable only by deliberately disabling
  GI in a test, now true unconditionally**: nothing artificially seeds the cache on enable, so a
  device's first-ever genuine change is *also* bootstrap-suppressed, exactly like a GI snapshot
  would have been — visibility resumes starting with the second transition, whose `previousValue`
  correctly reflects the first (silently-seeded) one. `integration_tests/mms_report_client/`'s
  tests each perform an explicit throwaway seed flip before the one they actually assert on,
  documenting this. This
  still solves the original "stable points must get their value to the frontend somehow" problem
  the way the previous revision described: the GI-seeded (or first-observation-seeded) cache value
  surfaces as `previousValue` on the first *genuine* change afterward (via
  `MmsReportEntry.previousValue`, an owned clone of the pre-update cache slot, captured
  unconditionally in `shouldForwardAndUpdateCache` regardless of its own forward/drop outcome —
  see that function's own doc comment) — so a point that never changes again still never reaches
  the frontend at all (an accepted, explicit tradeoff — see "IPC / Reporting Out"), but one that
  does change, even for the first time, reports a real "changed from X to Y" instead of an initial
  value with no context. If every entry in a report is filtered (including an all-bootstrap GI
  snapshot), `mms_report_client_report_adapter.c`'s `onReport` frees the record without ever
  invoking the caller's report callback (no empty/pointless push downstream) — this is the exact,
  already-existing mechanism that keeps a GI-only report from ever reaching the websocket,
  requiring no change to the report adapter itself. `goose_subscriber` mirrors the
  bootstrap-suppression side of this exactly (GOOSE has no reason-for-inclusion at all, so *every*
  candidate already went through this same "cached==NULL → bootstrap, don't forward" rule, and
  never needed a reason-trust bypass removed) — for the literal first-ever frame only; a
  STALE/INVALID_STATE→VALID liveness recovery's own first frame is now diffed against the real,
  preserved pre-outage cache instead of being unconditionally bootstrap-suppressed too (see the
  "Fifth change in the same family" bullet under `mms_report_client/`'s own Architecture entry
  above for the full redesign, which applies identically to `goose_subscriber`).
  **The per-position value-diff filter above, by itself, broke `ipc_dispatcher`'s quality pairing**
  (found against real production traffic): quality (`q`) almost never changes value
  report-to-report and rarely carries a real-change reason on a report triggered by its sibling
  value changing, so after the first `GI` snapshot `q`'s own diff-check drops it on every
  subsequent report while its value sibling (e.g. `stVal`) keeps forwarding — and since
  `ipc_dispatcher`'s `IpcDispatcherUseCases_pairQuality` only pairs entries present in that same
  record, quality showed as `null` forever after the first report. Symmetrically, a genuine
  quality-only change (no value change) left a lone `q` with no forwarded value sibling, which
  `pairQuality` also drops outright — a real quality-degradation event vanished silently too.
  Fixed by making `buildEntries` group-aware: it now runs in three phases (candidate
  collection → per-candidate value-diff-filter decision → a group-extension pass) instead of a
  single decide-and-emit pass. A candidate that didn't individually qualify still forwards if
  ANY other candidate resolving to the same group anchor does. One unified "any group member
  qualifies → forward the whole group" rule handles both directions (value drags quality along;
  quality drags its value sibling(s) along) with no special-casing of which DA is "the value" vs
  "the quality" — confirmed by the user's explicit choice to keep this full symmetry rather than
  narrow it, even knowing it means a sibling DA that doesn't change in lockstep with its group
  (e.g. `t`/`stSeld` on a real device that doesn't bump them on every `stVal` change) gets resent
  unchanged whenever any other group member does. A candidate whose reference doesn't resolve to
  any anchor at all is its own ungroupable singleton, falling back to the exact pre-existing solo
  diff-check.
  **Group anchor resolution is an ancestor walk, not a single-`$`-strip** (found against real
  measured-value traffic: a CMV's nested `cVal$mag$f` chain never paired with its own quality,
  which lives 3 `$`-segments shallower at the CMV instance's own level, e.g. `...phsA$q` vs
  `...phsA$cVal$mag$f` — a single last-`$` strip lands on `...phsA$cVal$mag`, never matching).
  Every `q`-named candidate's own prefix (up to its last `$`) becomes an "anchor"; every candidate
  (including `q` itself) resolves to the LONGEST anchor it's genuinely nested under (starts with
  `anchor$`) — a flat attribute (`Pos$stVal`) resolves directly to its DO's own `q`, exactly as
  before; a deeply nested CONSTRUCTED-DA chain resolves to the same DO/SDO-level anchor several
  segments up, not whatever its own last segment happens to be. Candidates resolving to the same
  anchor are one group. `ipc_dispatcher`'s `IpcDispatcherUseCases_pairQuality` has the identical
  ancestor-walk fix (`findQualityIndexForValue`) for the same reason, on the pairing side —
  both sides need it: this feature's grouping decides whether quality is even present in a given
  record, `ipc_dispatcher`'s pairing decides whether a present quality gets attached to the right
  value in the JSON. Proven via new unit tests in both features' usecases test files, covering
  the nested-CMV case, an anti-overreach case (two independent CMV instances under a shared
  higher ancestor must never cross-pair), both drag directions, the both-unchanged case, the
  ungroupable-singleton fallback, and the same grouping working across Gap 4 decomposition (two
  leaves of one DO-level FCDA, not just two independently-authored FCDA entries).
  Symmetrically, `goose_subscriber` used to forward a `GooseSubscriberRecord` on every accepted
  GOOSE frame, including ordinary `MinTime`/`MaxTime` heartbeat retransmissions (same `stNum`,
  incremented `sqNum`) that carry no actual data change — `GooseSubscriberUseCases_isDuplicateStNum`
  plus a new per-target `hasForwardedStNum`/`lastForwardedStNum` pair (`GooseSubscriberTargetEntry`,
  written only by the frame adapter thread, deliberately not under `targetStateLock` since the
  liveness thread never touches them) now skips forwarding when a frame's `stNum` matches the
  last one actually forwarded for that target; a STALE/INVALID_STATE→VALID transition resets this
  so the next real frame is always delivered at least once. Neither change touches
  `ipc_dispatcher`, which stays reason/stNum-unaware — filtering happens entirely upstream.
- **Third bugfix in the rollback/reconnect family, surfaced by real-hardware testing**: after the
  reason-trust removal above, a real device still showed the exact same flood of forwarded
  `MMS_REPORT` messages with `previousValue == value` return after being physically disconnected
  and reconnected. Root-caused to two compounding bugs in `mms_report_client_connection.c`, both
  now fixed, neither related to `ReasonForInclusion` (which was already fully untrusted by this
  point) — confirming `shouldForwardAndUpdateCache` itself was already byte-for-byte identical to
  `goose_subscriber`'s own equivalent, and the recurrence had a purely structural cause instead:
  (1) **reconnect "storm"**: `onStateChanged` posts `handle->wakeSignal` on *every* state
  transition, not just `IED_STATE_CLOSED` (`connectionLostSignal` is only set for `CLOSED`) — and
  `IedConnection_connect()` itself drives `CONNECTING` then `CONNECTED`, each posting once, so by
  the time `supervisorLoop` first reached its wait after a successful connect, wakes were already
  pending. The old code treated *any* wake with `connectionLostSignal == false` as "spurious, go
  reconnect anyway" (`continue` back to the top of the outer loop), so one real connect could
  trigger `enableAllTargets()` — a fresh RptEna cycle per RCB (this repo's own reference client
  requested GI too, at the time; GI has since been removed entirely — see the fourth bugfix
  below and `mms_report_client/`'s own Architecture bullet) — more than once, back to back,
  with nothing having actually been lost. Fixed by replacing that `continue` with an inner wait
  loop that stays in the same connected phase, consuming every wake, until a genuine
  `connectionLostSignal` (or stop) arrives — see `supervisorLoop`'s own comment for the full
  reasoning. (2) **unsynchronized cache access**: `enableOneTarget`'s
  `MmsReportClientUseCases_resetValueDiffCache` call runs on the supervisor thread, while
  `mms_report_client_report_adapter.c`'s `onReport` reads/mutates the exact same
  `lastForwardedValues` slots (via `MmsReportClientUseCases_buildReportRecord`) on libiec61850's
  own report-reader thread — `grep -rn "Mutex\|lock" src/features/mms_report_client` returned zero
  hits before this fix, so nothing prevented these two threads from racing on the same `MmsValue*`
  slots. Combined with (1)'s storm (more overlapping enable cycles means more contention windows),
  this is the most direct explanation for the exact symptom observed: a torn/use-after-free-style
  read of a cache slot concurrently being reset can end up aliasing the incoming value, defeating
  the diff check and forwarding a false "change" with a corrupted previous value. Fixed by adding
  a binary-mutex `Semaphore memberRefCacheLock` to `struct sMmsReportClientHandle` (created/
  destroyed alongside `wakeSignal`, same `Semaphore_create(1)` idiom as `goose_subscriber`'s own
  `targetStateLock`), held around both the reset call in `enableOneTarget` and the
  `buildReportRecord` call in `onReport`. **Why GOOSE never needed this**: `goose_subscriber`'s
  own reset-then-process happens synchronously, in one function call, on the single `GooseReceiver`
  reception thread (`GooseSubscriberFrameAdapter_onGooseReceived`) — no second thread ever touches
  its `lastForwardedValues`. MMS can't be restructured to match that shape (the reset is tied to an
  async `setRCBValues` write while reports arrive on a separate library-owned thread), so an
  explicit lock is what achieves the same mutual-exclusion guarantee GOOSE gets for free — this,
  not any further `reason`-based change, is what "make MMS filtering match GOOSE's" actually
  required once the domain-logic level was already confirmed symmetric. Proven via a strengthened
  `integration_tests/mms_report_client/` reconnect test asserting a single reconnect now produces
  exactly one additional RCB-enable event, not two-or-more.
- **Fourth bugfix in the same family, plus a deliberate scope-narrowing, both at explicit user
  request**: even after the third bugfix above, the reset in `enableOneTarget` still ran *after*
  `IedConnection_setRCBValues` returned, not before — a real, if narrower, race than (2) above:
  the write that enables reporting (and, at the time, requested GI) can itself trigger a report
  dispatched on libiec61850's own report-reader thread before the supervisor thread gets back
  around to resetting the cache a few lines later, diffing that report against a STALE
  (pre-disconnect, on reconnect) cache instead of a freshly-cleared one — plausibly explaining a
  real-device burst where everything looks "changed" right after a connect/reconnect, independent
  of the already-fixed storm/locking bugs. Fixed by moving the `memberRefCacheLock`-guarded
  `MmsReportClientUseCases_resetValueDiffCache` call to run immediately after
  `IedConnection_installReportHandler`, before the mask is built and `IedConnection_setRCBValues`
  is even called — no report for this RCB can be dispatched before the reset has already run,
  closing the window structurally rather than hoping the supervisor thread wins a scheduling race.
  Resetting unconditionally, even if the subsequent write fails, is harmless (nothing can report
  for a not-yet-enabled RCB either way). Separately, at the same request, GI was removed from this
  feature entirely rather than merely left untrusted (see `mms_report_client/`'s own Architecture
  bullet above for the full reasoning and the resulting test changes) — the two changes shipped
  together since both touch the same few lines of `enableOneTarget`.
- **Fifth change in the same family — the value-diff cache is now NEVER reset at all, on either
  `mms_report_client` or `goose_subscriber`, at explicit user request**: every bugfix above
  (storm/locking/reset-ordering) treated "reset the cache on every reconnect/recovery" as a given
  and fixed increasingly narrow races around *when* that reset ran — but the reset itself was the
  root of a separate, simpler problem: a device with a perfectly good last-known value lost it
  every time the connection blipped, since a freshly-nulled cache turns the reconnect's own GI/
  redelivered snapshot back into a bootstrap event (silently seeded, never forwarded,
  `previousValue` left `NULL`) even though a real prior value existed a moment before. The fix:
  `MmsReportClientUseCases_resetValueDiffCache`/`GooseSubscriberUseCases_resetValueDiffCache` are
  deleted entirely (zero remaining callers) — `enableOneTarget`
  (`mms_report_client_connection.c`) no longer resets anything before its enable write, and the
  frame adapter (`goose_subscriber_frame_adapter.c`'s `onGooseReceived`) no longer resets anything
  on a STALE/INVALID_STATE→VALID transition (it still resets the unrelated `hasForwardedStNum`
  heartbeat-dedup flag there, untouched). The cache is now populated **exactly once**, on a
  position's genuine first-ever observation, and **preserved for the rest of the client/
  subscriber's lifetime** (only freed at `STOP_REPORTING`/destroy, as before). GI (MMS) and the
  liveness-recovery mechanism (GOOSE) are otherwise unchanged — still forced/detected on every
  reconnect/recovery — but the fresh snapshot they produce now diffs against the **real, preserved
  last-known value** instead of a wiped-clean one: a genuine change made while disconnected now
  correctly forwards with a real, non-`NULL` `previousValue`; an unchanged resend is still
  correctly suppressed by the ordinary diff check, no bootstrap logic or reset-timing race
  involved. A new per-RCB/per-target `everPopulated` flag (`MmsReportClientMemberRefCacheEntry`/
  `GooseSubscriberMemberRefCache`, set once at the end of the first report/frame either feature's
  own `buildEntries` ever processes) exists purely to gate a new debug check in
  `shouldForwardAndUpdateCache` (both features): a cache slot found `NULL` *after* `everPopulated`
  is already `true` should now be structurally impossible (nothing ever resets a slot back to
  `NULL` again) — `fprintf(stderr, ...)` fires on every such occurrence, since it now signals a
  real bug worth investigating on sight, not routine startup noise. `memberRefCacheLock`
  (MMS)/`targetStateLock` (GOOSE) are unchanged — the MMS lock in particular is now technically
  guarding a single-writer scenario (the report-adapter thread is the cache's only remaining
  writer, since the supervisor thread no longer touches it), kept anyway as cheap, uncontended
  insurance. Proven via new unit tests in both features' usecases test files driving
  `buildReportRecord`/`buildRecord` multiple times in a row with no reset call anywhere in
  between, asserting: the first call silently seeds the cache and sets `everPopulated`; a
  simulated-reconnect/recovery call with a genuinely different value forwards with a real
  `previousValue`; an unchanged resend is suppressed exactly like any other duplicate.
- `ied_model`'s `IedModel_getGooseSubscriptionTargets` returns `GooseSubscriptionTarget*`
  (object reference plus optional VLAN/APPID/dst-MAC parsed from SCL's `<GSE><Address>`),
  not a bare `char*` — this was a breaking change made when `goose_subscriber` needed the
  addressing data to configure `GooseSubscriber_setDstMac`/`setAppId` filters; there were no
  other consumers at the time.
- `ied_model` also now exposes `IedModel_getDataSetMemberReferences` (ordered, heap-allocated
  reference strings for one dataset, purely local — walks the already-parsed SCL `DataSet`,
  never over-the-wire) and `GooseSubscriptionTarget` gained a `datasetReference` field
  (mirroring `ReportControlBlockTarget`'s) — both added so `mms_report_client`/`goose_subscriber`
  can label report/GOOSE entries by their dataset position. See "IPC / Reporting Out" below for
  the full reference/quality field-availability picture this closed.
- `integration_tests/ied_simulator/` is implemented: a small "Reporter1" fake IED
  (`src/sim_types.h`/`sim_server.c`) built directly via libiec61850's dynamic model API
  (not genmodel.jar codegen - see `scripts/generate_model.sh`'s comment for why, and where
  genmodel.jar actually lives in the sibling checkout if a larger simulated device ever
  needs it). Zero includes from `src/` - fully decoupled from prod code. `src/main.c` there
  is a standalone manual-testing binary (`cd integration_tests/ied_simulator && make run`,
  periodically flips its indication point); `src/sim_server.c` is also linked directly into
  `integration_tests/mms_report_client/e2e_test_mms_report_client.c` and
  `integration_tests/goose_subscriber/e2e_test_goose_subscriber.c` (client/subscriber and
  server run in the same test process over loopback/`lo` - libiec61850 supports this fine).
  It publishes both a buffered report (`brcbMain`) and GOOSE (`gcbInd`, on `lo`) over the same
  dataset (`ds1`, `GGIO1.Ind1.stVal`) — one `SimServer_setIndication` flip drives both E2E tests.
- **Two non-obvious libiec61850 dynamic-model gotchas hit while building the simulator**
  (worth knowing before touching `sim_server.c` or writing another dynamic-model-based
  server): (1) `DataSetEntry_create`'s variable reference is `"<lnName>$<fc>$<doName>$<daName>"`
  with **no** LD-wire-name prefix (confirmed against `libiec61850/examples/server_example_dynamic/`);
  including one makes the entry silently fail server-side resolution, which fails the whole
  dataset's access check and `RptEna` with `DATA_ACCESS_ERROR_OBJECT_VALUE_INVALID`.
  (2) `DataSet_create`'s `name` argument already gets `"<lnName>$"` prepended internally
  (`dynamic_model.c`) - pass the bare local name (`"ds1"`), not `"LLN0$ds1"`, or it double-prefixes
  and fails `IedModel_lookupDataSet`'s server-side match against a client-supplied `DatSet`
  reference (`DATA_ACCESS_ERROR_TEMPORARILY_UNAVAILABLE`).
- `third_party/lib` has `libiec61850.a`, `libhal.a`, `libmxml.a`, `libunity.a`, `libwebsockets.a`,
  and `libcjson.a` already built and vendored — do not rebuild these from source, link against
  them directly. `libunity.a` is test-only (Unity test framework) — never linked into the daemon
  binary itself, only into test binaries under `tests/`/`integration_tests/`.
- `third_party/include` has the full libiec61850 header set flattened in already (goose_receiver.h, goose_subscriber.h, mms_client_connection.h, reporting.h, etc.), plus `mxml.h`, `unity.h`/`unity_internals.h`, `cJSON.h`, and `libwebsockets.h` (+ its generated `lws_config.h` sibling, plus a `libwebsockets/` **subdirectory** of ~120 headers — unlike every other vendored header set, these are deliberately **not** flattened, because `libwebsockets.h` itself `#include`s them via `<libwebsockets/lws-*.h>` path-qualified angle-bracket includes; flattening would require hand-editing 100+ generated includes, so the one exception to the flat-vendoring convention is preserving this one subdirectory) — read these instead of guessing function signatures.
- `third_party/include/stdbool.h` is a broken vendored shim (`#define bool int`, no `true`/`false` — meant only for MSVC), no include guard, that can shadow the real system `<stdbool.h>`. Real builds use `-idirafter third_party/include` (never plain `-I`) so gcc prefers the real header, but IDE tooling (VS Code's C/C++ extension) doesn't reliably honor the same ordering through `includePath` alone. Don't hand-edit the vendored file — instead, any of our own code that uses `bool`/`true`/`false` should `#include "stdbool_compat.h"` (at `src/stdbool_compat.h`) instead of `<stdbool.h>` directly; it self-heals regardless of which `stdbool.h` a given toolchain resolves.
- `libmxml.a`/`mxml.h` (Mini-XML v3.3.1, Apache-2.0) is vendored for SCL (`.icd`/`.cid`/`.scd`) parsing — no XML/SCL parser exists elsewhere in this repo; do not hand-roll one. Source built from a sibling checkout at `/home/aleksa/code/ied_reporter/mxml` (not committed here — only the build artifacts are vendored, matching the libiec61850 pattern). Smoke test proving linkage + real SCL parsing: `tools/smoke_tests/mxml_smoke_test.c`.
- No libpcap/Npcap present in `third_party/` — that's a system dependency, link against the system lib, don't vendor it.
- Unity (ThrowTheSwitch/Unity, MIT) is vendored the same way as libiec61850/mxml: built into `libunity.a` and copied into `third_party/lib`/`third_party/include` (plus `unity_LICENSE.txt` for attribution) — not left as loose source. Source built from a sibling checkout at `/home/aleksa/code/ied_reporter/Unity` (not committed here, same convention as mxml's build). Test binaries link `-lunity`, they don't compile `unity.c` themselves. **This vendored build has double-precision assertions excluded** (`TEST_ASSERT_EQUAL_DOUBLE`/`_FLOAT` fail with "Unity Double Precision Disabled") — compare floating-point values with a plain C `==`/epsilon check instead, not a Unity float/double macro (see `tests/ipc_dispatcher/test_ipc_dispatcher_value_codec.c` for the pattern).
- `libwebsockets.a`/`libwebsockets.h` (libwebsockets, MIT) is vendored for `ipc_dispatcher`'s websocket transport — no websocket implementation exists elsewhere in this repo; do not hand-roll one for production code (the E2E test's minimal client is the one deliberate exception — see `ipc_dispatcher/`'s own bullet below for why). Source built from a sibling checkout at `/home/aleksa/code/ied_reporter/libwebsockets` (not committed here, same convention as mxml/Unity's build) via CMake with `-DLWS_WITH_SSL=OFF -DLWS_WITHOUT_EXTENSIONS=ON -DLWS_WITH_SHARED=OFF -DLWS_WITH_STATIC=ON -DLWS_WITHOUT_TESTAPPS=ON` (no TLS needed — internal-only, loopback). Link flag is `-lwebsockets` (the archive is named `libwebsockets.a`, not `liblws.a`).
- `libcjson.a`/`cJSON.h` (cJSON, MIT) is vendored for `ipc_dispatcher`'s JSON serialization — no JSON library exists elsewhere in this repo; do not hand-roll one. Source built from a sibling checkout at `/home/aleksa/code/ied_reporter/cJSON` (not committed here, same convention) via CMake with `-DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF` (cJSON's own `BUILD_SHARED_AND_STATIC_LIBS` option does **not** suppress the shared build by itself — `BUILD_SHARED_LIBS=OFF` is the flag that actually produces a static-only `libcjson.a`). Serialize-only everywhere except one deliberate exception: `control_dispatcher/`'s
  `data/control_dispatcher_json_parser.c` is this codebase's first production `cJSON_Parse` call
  (it must parse untrusted inbound `START_REPORTING`/`STOP_REPORTING` commands off the wire — see
  its own Architecture bullet above) — every other feature (`ipc_dispatcher`, `scan_dispatcher`)
  remains push-only/serialize-only, and cJSON's own parser is otherwise only used by tests, to
  assert JSON shape without brittle string matching.
- `build_out` at repo root is currently a stray compiled binary from a manual verification build, not an empty directory — this line is inaccurate until cleaned up or the doc is corrected; don't rely on it being a directory.
- **Include convention**: one project-wide include root, `-Isrc`, for all our own code — every intra-project `#include` is fully qualified from `src/` (e.g. `#include "features/ied_model/service/ied_model_api.h"`), never a bare filename or `../` relative path. Vendored third-party headers stay bare filenames against `-idirafter third_party/include` (see above). `.vscode/c_cpp_properties.json`/`tasks.json` are kept in sync with this — update both if the include strategy ever changes.

## Testing
- `tests/<feature>/test_*.c` — strict unit tests (Unity framework), one file per source
  file (e.g. `tests/ied_model/test_ied_model_api.c` tests `ied_model_api.c`). Fast,
  hermetic: fixtures are tiny in-memory models/mxml nodes built directly via the
  dynamic model API, or handles constructed directly (`struct sIedModelHandle` is
  visible to test code) rather than mocked. `tests/Makefile` is an explicit `TESTS` list
  plus one hand-written build rule per binary — it does **not** auto-discover, so a new
  feature's tests need a Makefile edit, not just new files. A feature's data-layer/loader
  code is deliberately *not* unit-tested here if its correctness is better proven
  end-to-end (see `ied_model`) — don't duplicate that coverage, extend the E2E test
  instead. Two self-contained temp-file cases exist (`tests/ied_model/test_ied_model_api.c`,
  `tests/orchestration/test_orchestration_staging.c`) — the only file I/O permitted here.
  `tests/ied_discovery/` covers pure CIDR math (`test_ied_discovery_cidr.c`, no real network),
  real `getifaddrs()` integration against loopback only (`test_ied_discovery_netif.c`, plus one
  deterministic `maxHosts`-ceiling assertion in `test_ied_discovery_api.c` — loopback's real
  netmask has far more hosts than the default ceiling, so this proves the safety valve without
  ever sending a network probe), and argument-validation wiring — never a real subnet scan or
  MMS association, that's `integration_tests/ied_discovery/`'s job.
  `tests/scan_dispatcher/` covers the duplicated ring-buffer/JSON-writer/api wiring the same way
  `tests/ipc_dispatcher/` covers its own (including a real loopback bind, same "no external
  dependency to avoid" rationale). `tests/scan_orchestration/` covers seen-set dedup logic plus a
  full start/stop/scanId-monotonicity/refcounting lifecycle driven against a deliberately
  nonexistent interface (`"nonexistent0"`) — every sweep fails fast and is tolerated gracefully,
  proving the lifecycle without needing any real reachable network; a real device actually being
  found and streamed is `integration_tests/scan_dispatcher/`'s and
  `integration_tests/scan_orchestration/`'s job instead.
  `tests/device_manager/` covers the port allocator (alloc/free/reuse/exhaustion on a tiny
  range, hermetic) and the two-phase-locked registry (reserve/finalize/rollback/removeIfRunning/
  host-dedupe transitions, driven directly against the registry's own public API with a fake,
  never-dereferenced `OrchestrationHandle` pointer — the registry only stores/hands back the
  pointer, never touches it) plus argument-validation wiring for the public API — never a real
  `Orchestration_run*` call, that's `integration_tests/device_manager/`'s job.
  `tests/control_dispatcher/` covers the duplicated ring-buffer wiring the same way
  `tests/ipc_dispatcher/`/`tests/scan_dispatcher/` cover their own, plus the request queue
  (push/pop/full/empty/fifo-order), the JSON writer/parser (envelope shape, malformed/missing/
  wrong-typed field cases — the parser is this codebase's first production `cJSON_Parse` call,
  see `control_dispatcher/`'s own Architecture bullet below), dispatch/error-mapping (driven
  against a real `DeviceManagerHandle` in deliberately-failing, no-network cases only), and a
  real bind/start/stop lifecycle for the API layer — never a real websocket frame sent, that's
  `integration_tests/control_dispatcher/`'s job.
- `integration_tests/<feature>/` — E2E tests of a real feature's public API against a
  real, self-authored fixture file (e.g. `integration_tests/ied_model/fixtures/breaker1.cid`
  + `e2e_test_ied_model.c`), also Unity-based (`-lunity` against the vendored
  `third_party/lib/libunity.a`). Unlike `ied_simulator/` (below), these intentionally
  link against real `src/` code — the decoupling rule is specifically about the IED
  simulator acting as a fake external device, not about integration tests of our own
  features.

## Architecture — Feature-First
- Repo root is the daemon. `integration_tests/ied_simulator/` holds IED simulators,
  fully decoupled from prod source; other `integration_tests/<feature>/` dirs hold
  E2E tests of real features (see Testing above) and do link against `src/`.
- Inside `src/features/<feature_name>/` (to be created as features are built):
  - `*_types.h` — domain entities, enums, opaque pointers (encapsulation boundary)
  - `*_api.h` — public service-layer API (orchestrator entry points)
  - `*.c` — implementation, third-party integration, static helpers
  - For features with real business logic worth isolating from third-party integration
    (see `ied_model/`), the above nests inside `domain/`/`data/`/`utils/`/`service/`
    subfolders: `domain/` holds `*_types.h` + pure logic with no third-party includes,
    `data/` holds third-party/file/library integration, `utils/` holds shared helpers,
    `service/*_api.h` is still the one public header other features may include. Simple
    features can skip the subfolders and just use the three files directly.
- `src/main.c` wires dependencies only — no business logic.
- Expected features: `scl_bootstrap/`, `ied_model/`, `goose_subscriber/`, `mms_report_client/`,
  `ipc_dispatcher/` — all five now implemented; don't invent unrelated features beyond these
  without being asked. `ied_discovery/` is a sixth feature added later, explicitly at user
  request (LAN auto-discovery of candidate IEDs) — not a case of inventing scope unprompted, see
  its own bullet below. `ied_model_online_loader/` is a seventh feature, also added later at
  explicit user request (a real device, e.g. OMICRON IED Scout's "Simulate IED" mode, that
  associates fine but never serves an SCL file over MMS file services at all) — see its own
  bullet below and the "No over-the-wire tree discovery" Hard Rule's documented exception.
  `scan_dispatcher/` is an eighth feature, also added later at explicit user request (turning
  network scanning into a continuously-running background process with its own streamed-out
  websocket, managed by start/stop entry points ahead of the real external API layer that will
  eventually drive them) — see its own bullet below.
  `control_dispatcher/` is a ninth feature, also added later at explicit user request (turning
  reporting itself into a multi-device, callable-action service — "start reporting on device X",
  "stop reporting on device Y" — instead of one fixed device per process run) — see its own
  bullet below.
  `src/orchestration/`, `src/scan_orchestration/`, and `src/device_manager/` are three separate,
  top-level siblings of `src/features/` (not themselves in this feature list) that each sequence
  a pipeline together — see their own bullets below. `ipc_dispatcher`'s lifecycle is owned
  entirely by orchestration (not by `src/main.c` directly) — `main.c` only ever configures it via
  `OrchestrationConfig.ipcDispatcherConfig`, same as every other feature's config;
  `scan_dispatcher`'s lifecycle is likewise owned entirely by `scan_orchestration` (reference-
  counted by active-scan count); `control_dispatcher`'s lifecycle is owned by `main.c` directly
  (unlike the other two dispatchers) since it's a single, shared, always-on control channel, not
  something any per-pipeline layer starts/stops on the caller's behalf — see `device_manager/`'s
  and `control_dispatcher/`'s own bullets below.
- `scl_bootstrap/` (implemented) — one-shot, synchronous bootstrap/probe utility: given a
  caller-supplied list of candidate host addresses, TCP-probes each for MMS on a given port,
  and for each one found, browses its file directory and fetches one SCL file (`.icd`/`.cid`/
  `.scd`/`.ssd`/`.sed`) over standard MMS file services, returning the raw bytes + filename.
  Does **not** load anything into `ied_model` and does **not** enable GOOSE/MMS reporting —
  purely discovery. Public boundary: `src/features/scl_bootstrap/service/scl_bootstrap_api.h`.
  Unlike `mms_report_client`/`goose_subscriber`, there is no `_start`/`_stop` pair —
  `SclBootstrap_scanAndFetch` blocks and returns the complete, authoritative result set (one
  `SclBootstrapResult` per input host, including the ones that didn't pan out) in one call;
  scanning a network is fundamentally a do-it-once, get-a-complete-answer operation, unlike the
  other two features' long-running background workers. Optionally gated behind ACSE password
  auth (one retry) via `SclBootstrapConfig.acseAuthPassword`. Proven end-to-end (including the
  auth-retry and negative-path cases) against a real `ied_simulator` IED in
  `integration_tests/scl_bootstrap/`. Also exposes `SclBootstrap_tcpProbeOnly` (thin wrapper
  around the same `SclBootstrapTcpProbe_scan` machinery `scanAndFetch`'s own phase 1 already
  uses) as a public entry point purely so `ied_discovery` can reuse its bounded-concurrency
  async TCP-probe state machine without pulling in the full MMS-association + SCL browse/fetch
  — that async state machine is substantial enough to be worth reusing rather than duplicating,
  unlike the small ACSE-auth-setup snippet `mms_report_client`/`ied_discovery` each duplicate.
- `ied_model/` (implemented) — loads an IED's data model from SCL (`.icd`/`.cid`/`.scd`), gated by an `AccessMode` (REPORT_ONLY/READ_ONLY/READ_AND_WRITE). Public boundary: `src/features/ied_model/service/ied_model_api.h`. `goose_subscriber`/`mms_report_client` should get their subscription targets from here, not by re-parsing SCL themselves. `IedModel_getReportSubscriptionTargets` returns `ReportControlBlockTarget*` (object reference with the correct `.RP.`/`.BR.` segment, buffered flag, dataset reference) rather than a bare string, specifically for `mms_report_client`'s use; `GooseSubscriptionTarget` carries the equivalent `datasetReference` for `goose_subscriber`. `IedModel_getDataSetMemberReferences(handle, datasetReference)` returns the ordered, heap-allocated member-reference strings backing one dataset (index i matches the i-th report/GOOSE entry) — purely local, walks the already-parsed SCL `DataSet`, never over-the-wire (see Hard Rules) — this is what both consumers use to label entries by position. Also exposes `IedModel_listIedNames(path, outError)` — lists every `<IED name="...">` at an SCL file's top level without building a full model, for `orchestration`'s optional IED-name auto-detection (see that feature's own bullet below); a file with zero `<IED>` elements is a valid, non-error, empty result. **Hardened against real-world SCL variation** (found integrating against a real Siemens SIPROTEC device and its exported station SCD): `VLAN-ID`/`APPID` are parsed as hex, not decimal-defaulting `strtoul` base-0 (real values like `"000A"` were silently corrupted to `0` under octal autodetection); `<GSE>`'s `MinTime`/`MaxTime` are now read instead of always defaulting; `<SDI>`-wrapped (structured/array) `<DOI>`/`<DAI>` overrides are now recursed into instead of silently dropped; enumerated `<DAI>` `Val` labels are resolved against the DA's real `<EnumType>` ordinal instead of `atoi`'d (a non-numeric label like `"status-only"` used to silently become ordinal `0`, itself a valid-looking wrong value, not a skip); `LDevice/@ldName` (SCL functional naming) is read and threaded into FCDA/LDevice resolution as a third fallback convention. A vendor pattern where control blocks are embedded as escaped text inside `<Private type="...ControlBlockStorage...">` (seen in a raw, unconfigured Siemens device-type template) is detected and warned about rather than silently producing an empty model — actually parsing that escaped payload is out of scope (vendor-specific, speculative). Deliberately **not** hardened, considered and deferred pending real evidence: duplicate `LDevice/@inst` across multiple `<AccessPoint>`s, `DAI/@ix` array indices, `<Val sGroup="N">` setting-group overrides, dotted `doName`/`daName` FCDA shorthand, non-dash-separated MAC address formats. `GSEControl`'s `datSet` was reconsidered for symmetry with `ReportControl`'s now-optional one and deliberately kept required — every real `GSEControl` sample encountered populates it. **Also exposes `IedModel_getDataSetMemberLeafWireTypes`** (mirrors `_getDataSetMemberLeafSemantics` exactly — reads each leaf's already-known `DataAttributeType` directly off its `DataAttribute` node, no new SCL-parsing pass) **plus `IedModel_dataAttributeTypeMatchesMmsType`**, added after a real-hardware finding that the Gap-4 structure-decomposition zip in `mms_report_client`/`goose_subscriber` needed a per-leaf type cross-check, not just the pre-existing count check — see `mms_report_client/`'s own bullet below for the full story.
- `mms_report_client/` (implemented) — connects to one IED over MMS, discovers its Report Control Blocks via `ied_model` (never re-parses SCL, never discovers RCBs over the wire), enables reporting on each (`RptEna`, **plus `DatSet`** using `ReportControlBlockTarget.datasetReference` — relying on a server-side default dataset configured only at RCB-creation time turned out to be fragile/version-dependent in practice, so the client always (re-)asserts it explicitly on every enable, matching libiec61850's own reference client example; `TrgOps`/`BufTm`/`IntgPd`/`ConfRev` are still left untouched, exactly as the IED's SCL config has them — **`GI` is requested on every enable**, purely to seed the value-diff cache deterministically, see below for the full history: removed entirely once, then reinstated for this narrow reason), and delivers normalized `MmsReportRecord`s via a caller-registered callback (JSON stringification is deferred to `ipc_dispatcher` — no JSON library is vendored). `MmsReportEntry.reference` prefers the server's own `ClientReport_getDataReference` (only present if the RCB's `OptFlds` has `DataRef` set) and falls back to a per-RCB cache of `IedModel_getDataSetMemberReferences` results (built once at `MmsReportClient_start`, never rebuilt on reconnect) when the server omits it. Works under every `ied_model` `AccessMode`, including `REPORT_ONLY`. Public boundary: `src/features/mms_report_client/service/mms_report_client_api.h`. Reconnects with exponential backoff via a dedicated supervisor thread (`hal_thread.h`'s `Thread`/`Semaphore`) driven by `IedConnection`'s state-changed handler — see that header's own doc comments for why the handler can't drive reconnection directly (deadlock risk). MMS host/port are caller-supplied (SCL parsing of the MMS `<ConnectedAP>` IP address is out of scope for now — only GOOSE addressing is parsed by `ied_model`). **Supports ACSE password authentication** via `MmsReportClientConfig.acseAuthPassword` (`data/mms_report_client_auth.c`'s `MmsReportClientAuth_configurePasswordAuth`, same third-party calls as `scl_bootstrap`'s own `data/scl_bootstrap_auth.c` — duplicated rather than shared, since features never reach into each other's `data/`/`domain/` layers, only `service/*_api.h`). `NULL` (default) means every association is unauthenticated, unchanged from before this was added. Unlike `scl_bootstrap` (which tries unauthenticated first, then retries once with a password only on rejection, since it's scanning candidates blind), `mms_report_client` applies the configured password unconditionally from the very first connect attempt — it always targets one already-known IED, so there's no ambiguity to resolve with a retry. Applied once, at `MmsReportClientConnection_create` time, to the one `IedConnection` object that's reused across every reconnect (unlike `scl_bootstrap`'s fresh-connection-per-attempt design), so it covers every future reconnect automatically. Proven end-to-end against a real `ied_simulator` IED in `integration_tests/mms_report_client/`, including both a correct-password-connects and a wrong-password-never-connects case against a real `SimServer_requireAuthentication`-protected instance. **`MmsReportEntry` also now carries `previousValue`** (see the value-diff filter bullet above and "IPC / Reporting Out" below for the full change-stream rework these back) — an owned clone of the pre-update value-diff cache slot, built alongside the existing `lastForwardedValues` cache in `buildMemberRefCache`.
  **`MmsReportClientConfig.generalInterrogationOnEnable` was removed entirely** (it used to
  default to `true`) — GI proved unreliable on real hardware even after its `reason` bit stopped
  being trusted for filtering (see the value-diff filter bullet above): a device has been
  observed tagging a GI-triggered snapshot `DATA_CHANGE` too, and requesting GI at all added a
  round-trip this client doesn't need, since `shouldForwardAndUpdateCache`'s own bootstrap
  suppression (any first observation for a position, whatever naturally produces it, is silently
  seeded but never forwarded) already gets the same "don't flood the frontend with the initial
  snapshot" outcome without asking the device for one. `enableOneTarget` now only ever sets
  `RCB_ELEMENT_RPT_ENA`[`|RCB_ELEMENT_DATSET`] — matching `goose_subscriber`'s own GI-less design
  exactly (GOOSE has no GI concept at all). At the time this GI-removal-then-reinstatement saga
  played out, `enableOneTarget` also reset the RCB's value-diff cache on every (re-)enable,
  moved to run BEFORE the enable write rather than after (closing a race window between the write
  triggering a report and the supervisor thread getting around to resetting). **That reset
  mechanism has since been removed entirely** (see the "Fifth change in the same family" bullet
  above, under the reconnect/rollback bugfix family) — the cache is now populated once and
  preserved forever instead, so there is no reset-timing race left to close; the reconnect test
  in `integration_tests/mms_report_client/` referenced below should be read with that superseding
  design in mind.
  **Dynamically creates a dataset for RCBs whose SCL declares no `datSet` at all** (`datSet="Dyn"` in SCL `<ReportSettings>` terms — confirmed against a real device, `E13_6MD`/`IEC 61850v2 JA4 station.scd`: every one of its ~174 `ReportControl` elements omits `datSet`, and RCBs there are parented under the specific LN they report on, not just `LLN0`, contradicting the earlier assumption that an RCB's parent LN is always `LLN0`). Previously this feature deliberately never created datasets itself (`setRCBValues` just failed with `IED_ERROR_OBJECT_VALUE_INVALID`, logged, RCB skipped) — that stance blocked reporting entirely on this whole class of device. Now, `data/mms_report_client_connection.c`'s `getOrCreateDynamicDataset` (called from `enableOneTarget` only when `target->datasetReference` is NULL) synthesizes an association-scoped dataset (`IedConnection_createDataSet` with an `@`-prefixed name — destroyed automatically when the connection closes, so no explicit cleanup/leak risk across reconnects) covering **every FC=ST/MX leaf attribute under the RCB's own LN** — "all the variables" for that LN, by this codebase's existing FC=ST/MX "reportable" convention (see `IedModel_getReadTargets`). The member list comes from a new `ied_model` accessor, `IedModel_getReportableAttributeReferencesForLogicalNode(handle, lnReference)` (`ReportControlBlockTarget` gained an `lnReference` field for this), purely local like every other `ied_model` accessor — never over-the-wire. `mms_report_client_api.c`'s `buildMemberRefCache` uses this same accessor (not just the connection layer) to seed the RCB's reference-labeling/value-diff cache up front, so dynamic RCBs get the exact same reference-labeling/value-diff-filter treatment as SCL-declared ones, no special-casing downstream. A new domain usecase, `MmsReportClientUseCases_buildWireMemberReferences`, converts this codebase's standard `"$"`-joined reference form to `IedConnection_createDataSet`'s required dot/bracket wire form. A per-connect-cycle cache (LN reference → generated dataset name, built fresh in `enableAllTargets`, discarded at the end) de-dupes dataset creation across an LN's redundant reserved RCB instances (e.g. `urcbA..urcbJ` all sharing one LN) — without it, a device like `E13_6MD` would attempt to create the same dataset ~10× over just for one LN's reserved slots. **Known, deliberately unsolved limitations**: no chunking against a device's `maxAttributes` cap (an LN with more reportable leaves than the cap fails `createDataSet` for that LN, falls back to the pre-existing failure mode); no handling of a device's total dataset-count cap being smaller than its unique-LN count (per-LN scope, not per-LDevice) — both are honest, unresolved trade-offs from a design discussion that intentionally deferred multiple stakeholder-specific scope questions rather than guessing. Proven end-to-end against a real `ied_simulator` IED in `integration_tests/mms_report_client/` (a fixture RCB parented under a non-`LLN0` LN, no `datSet` at all, mirroring `E13_6MD`'s real shape).
  **GI was later reinstated, at explicit user request, after real-world use surfaced a gap the
  removal above didn't account for**: the removal's own reasoning ("bootstrap suppression alone
  gets the same outcome without asking the device for one") assumed *some* report always arrives
  to naturally seed `shouldForwardAndUpdateCache`'s cache first. That's false for a device with no
  periodic integrity reporting (`IntgPd`) and no other traffic at enable time — without GI, the
  cache is only ever seeded by whatever report happens to arrive first, and on such a device
  that's the very first GENUINE value change after connecting. That change gets silently
  bootstrap-suppressed right along with it (the exact same mechanism that correctly suppresses a
  real GI snapshot), so only the *second* change onward was ever visible — a real device with this
  reporting profile would appear to the frontend as if the filter were dropping legitimate
  changes, because it was. `enableOneTarget` (`data/mms_report_client_connection.c`) now
  unconditionally sets `RCB_ELEMENT_GI` and calls `ClientReportControlBlock_setGI(rcb, true)` on
  every enable — first connect and every reconnect alike — no config knob (unlike the pre-removal
  `generalInterrogationOnEnable` bool this codebase once had; nothing asked for a way to turn it
  off this time). This is a narrow, deliberate use of GI: it exists ONLY to force an immediate,
  deterministic snapshot at enable time, and that snapshot is NEVER trusted or forwarded — it lands
  on the exact same `cached == NULL` bootstrap-suppression branch any other first observation
  would, in `shouldForwardAndUpdateCache`. At the time this reasoning was written, the cache was
  also reset immediately before this write on every enable, so GI's snapshot was always diffed
  against a freshly-cleared cache — **that reset no longer exists** (see the "Fifth change in the
  same family" bullet above): the cache is populated once and preserved forever, so on a genuine
  first-ever connect GI's snapshot still lands on the same `cached == NULL` bootstrap branch (the
  cache is still empty at that point), but on every reconnect after that, GI's snapshot instead
  diffs against the real, preserved last-known value from before the disconnect. Re-adding GI is
  safe this time specifically because the OTHER half of the
  original bugfix — never trusting a report's `reason` bit for filtering, see
  `shouldForwardAndUpdateCache`'s own doc comment — was never touched or weakened; that, not GI's
  absence, is what made the original real-hardware flooding bug possible, and it still applies
  unconditionally to every report regardless of source (this client's own requested GI, a foreign
  client's concurrent GI, or a buffered RCB's redelivered backlog). On a genuine first-ever
  connect specifically, the still-empty cache may receive both a GI-triggered snapshot AND a
  buffered RCB's redelivered backlog carrying the same live value in either order — whichever
  lands first hits the
  `cached == NULL` branch and seeds the cache, whichever lands second is then a byte-identical
  duplicate and is suppressed by the ordinary value-diff check instead; neither ever reaches the
  callback either way, so this is order-independent by construction, not a race the implementation
  has to win. `goose_subscriber` is untouched — it has no GI concept at all, and its own
  `cached == NULL` bootstrap suppression (already order-independent for the same structural reason)
  needed no equivalent change. `integration_tests/mms_report_client/`'s tests were updated to match
  the new behavior: the "throwaway seed flip" pattern each test previously used to manually stand
  in for GI's absence is gone — since GI now seeds the cache deterministically at enable time, the
  first flip in each test is itself a real, immediately-forwarded change; the reconnect test's
  redelivery-suppression assertion is unchanged in outcome but its comment now explains the
  GI/buffered-redelivery order-independence above.
  **A second, distinct real-hardware bug was found shortly after the GI reinstatement above,
  against a real production device**: right at connect, a buffered RCB forwarded the SAME report
  content 3 times in a row — `value == previousValue` on every field — before settling into
  correct filtering. Root-caused to `shouldForwardAndUpdateCache`'s diff-check
  (`MmsReportClientUseCases_isDuplicateValue`, and the identical cross-RCB-dedup call in
  `MmsReportClientUseCases_shouldForwardAcrossRcb`) calling libiec61850's `MmsValue_equals`
  directly — a raw, byte-exact comparison, confirmed by reading the vendored source, that's wrong
  for two IEC 61850 types that show up constantly in real report datasets: **`MMS_UTC_TIME`**
  (`memcmp`s all 8 bytes, but the last byte is a `TimeQuality` flag — leap-second-known/
  clock-failure/clock-not-synchronized/accuracy — not part of "when did this happen," and can
  legitimately wobble right around a reconnect even though the displayed millisecond timestamp is
  unchanged) and **`MMS_BIT_STRING`** (the wire encoding for CODEDENUM/Dbpos/Tcmd-style status
  points — `memcmp`s the whole buffer INCLUDING unused padding bits, which real device firmware is
  commonly inconsistent about zero-padding across different report-generation code paths, e.g. a
  GI-triggered read vs. a live-change report — two values that decode to the identical integer,
  and thus render identically in the JSON, can still fail a raw `memcmp`). Fixed by a new
  `valuesAreSemanticallyEqual` (`domain/mms_report_client_usecases.c`) that type-switches: same
  type required (preserves existing `MmsValue_equals` type-mismatch behavior — not the bug);
  `MMS_UTC_TIME` compared via `MmsValue_getUtcTimeInMs` (the same accessor `ipc_dispatcher`'s own
  value codec already uses to render this type, so "same JSON output" now correctly implies "same
  by this filter" too); `MMS_BIT_STRING` compared via `MmsValue_getBitStringSize` (a guard) plus
  `MmsValue_getBitStringAsInteger` (again, the same accessor the value codec already uses); every
  other type falls through to `MmsValue_equals` unchanged. **Affects `goose_subscriber` identically**
  (`GooseSubscriberUseCases_isDuplicateValue` and `shouldForwardAcrossTarget` both called
  `MmsValue_equals` directly too — GOOSE frames carry the same DA types) — fixed with an
  independently-duplicated copy of the same helper in `goose_subscriber_usecases.c`, per this
  codebase's established per-feature-domain-layer convention. Proven via new unit tests
  (`tests/mms_report_client/test_mms_report_client_usecases.c`,
  `tests/goose_subscriber/test_goose_subscriber_usecases.c`) constructing same-value-different-
  quality-byte `MMS_UTC_TIME` pairs and same-decoded-integer-different-size `MMS_BIT_STRING` pairs
  directly — **note the exact real-world byte pattern (same declared size, differing UNUSED padding
  bits) can't be reproduced via the public `MmsValue` API**: `MmsValue_setBitStringBit` itself
  refuses to touch bit positions ≥ the declared size (confirmed directly in libiec61850's own
  source), so every `MmsValue` constructible via the public API has its padding bits permanently
  zeroed by `MmsValue_newBitString`'s own `calloc` — that gap only exists in a real device's own
  wire encoding, not in anything reachable through well-behaved client code, which is exactly why
  it was a genuine, hard-to-suspect field bug rather than something a normal test would catch.
  Separately (not the cause of the wrong comparison, but why the burst happened specifically at
  connect and then stopped): `supervisorLoop` used to reset `currentBackoffMs` to `0`
  unconditionally on every *momentary* successful connect, before the connection proved it could
  stay up — a real, flaky link that connects then bounces right back (unlike the clean loopback
  simulator, which never does this) got stuck retrying at the initial ~1s backoff tier forever
  instead of escalating, giving the comparison bug repeated fresh chances to fire in a tight burst
  right after connect. Fixed by only resetting `currentBackoffMs` if the just-lost connection had
  actually stayed up for `MMS_REPORT_CLIENT_STABLE_CONNECTION_MS` (`5000`ms) — reuses the existing,
  already-tested exponential-backoff math (`MmsReportClientUseCases_computeNextBackoffDelay`)
  unchanged, just fixes *when* it resets rather than inventing a new debounce mechanism.
  **A third, distinct real-hardware bug was found shortly after, also surfacing after
  disconnect/reconnect cycles on real hardware**: a structured attribute (`Pos`, a DPC on LN
  `SCSWI2`) had its `stVal`/`t` sub-elements swapped — `stVal` showed a huge millisecond-timestamp
  number, `t` showed a plain boolean. Root-caused to the Gap-4 structure-decomposition path
  (`collectCandidates`, mirrored in `goose_subscriber_usecases.c`): it flattens a structured
  attribute's wire value (`MmsReportClientUtils_flattenStructure`, walking the received
  `MmsValue`'s own `MMS_STRUCTURE` element order) and zips it index-for-index against a
  **locally-resolved** reference list built from this daemon's own parsed SCL file
  (`IedModel_getDataSetMemberLeafReferences`, a depth-first walk of the SCL `<DOType>`'s literal
  XML `<DA>`/`<SDO>` child order). The only safety check before trusting this zip was
  `flattenedCount == memberLeafCounts[i]` — a bare **count** comparison; nothing verified
  **order**, and nothing in MMS/IEC 61850 requires a `<DOType>`'s declared `<DA>` order to match a
  real device's actual runtime attribute order (confirmed: `ClientReport_getDataReference`/
  `OptFlds` `DataRef` cannot help here either — it's fundamentally per-top-level-dataset-member
  (per-FCDA) only, with no protocol-level concept of a reference for a leaf *inside* a structured
  value). This same-count-different-order gap was already documented as an *assumption* in
  `IedModelUseCases_getDataSetMemberLeafReferences`'s own doc comment, but had no actual guard.
  Fixed with a new `ied_model` accessor, `IedModel_getDataSetMemberLeafWireTypes` (mirrors
  `_getDataSetMemberLeafReferences`/`_getDataSetMemberLeafSemantics` exactly — same index-aligned,
  same decomposed-vs-leaf split — but reads each leaf's already-known `DataAttributeType` directly
  off its `DataAttribute` node, set once at SCL-load time via `IedModelUtils_mapBType`; no new
  SCL-parsing pass needed, since `struct sDataAttribute` already stores this field, fully exposed
  in `iec61850_model.h`) plus a new `IedModel_dataAttributeTypeMatchesMmsType(DataAttributeType
  expected, MmsType actual)` cross-check, called once per decomposed leaf
  (`decomposedLeafTypesMatch`, both `mms_report_client_usecases.c` and its `goose_subscriber`
  twin) alongside the existing count check — on ANY leaf's type mismatch, falls back to the raw/
  non-decomposed entry exactly like a count mismatch already did (silently — same posture, no new
  logging), rather than trusting a same-count-wrong-order zip. Only implements **confident**,
  well-established type groupings (BOOLEAN↔`MMS_BOOLEAN`, TIMESTAMP↔`MMS_UTC_TIME`,
  QUALITY/CODEDENUM/CHECK/GENERIC_BITSTRING/OPTFLDS/TRGOPS↔`MMS_BIT_STRING`, the INT*/FLOAT*/
  ENUMERATED family↔`MMS_INTEGER`/`MMS_UNSIGNED`/`MMS_FLOAT`, VISIBLE_STRING*/UNICODE_STRING_255↔
  `MMS_VISIBLE_STRING`/`MMS_STRING`) — per this codebase's "don't guess IEC 61850 semantics" rule,
  anything not explicitly modeled (`IEC61850_UNKNOWN_TYPE`, `OCTET_STRING_*`, `ENTRY_TIME`,
  `PHYCOMADDR`, `CURRENCY`, `CONSTRUCTED`) always matches (no check), rather than risk
  false-positive-rejecting a genuinely well-ordered structure. Proven via new unit tests in both
  `tests/ied_model/` (the new accessor + the type-compatibility matrix) and both
  `mms_report_client`/`goose_subscriber` usecases test files (a same-count-different-order fixture
  — a UTC_TIME value and a boolean swapped into a `stVal`/`t`-labeled pair, exactly reproducing
  the real-hardware symptom — asserting fallback to the raw entry, plus a regression case proving
  a genuinely-matching order still decomposes normally). No new E2E test: `ied_simulator` builds
  its dynamic model directly from the same code that produces its own reference list, so wire
  order and reference order are inherently always in sync there — this bug class can only be
  exercised at the unit level with hand-built fixtures, which is what the existing Gap-4
  decomposition tests already do. Also plausibly explains the "oscillating quality" symptom
  observed alongside this on the same device: `q` and `stVal` (Dbpos-coded) are both wire-typed
  `MMS_BIT_STRING`, so if the order mismatch zipped the `...Pos$q` reference to what was actually
  the wire's `stVal` position, `ipc_dispatcher` would still successfully decode it as quality (it
  only type-checks for `MMS_BIT_STRING`, which still passed) — just decoding the wrong bits; not
  independently confirmed, would need real-hardware verification once this fix is deployed.
- `goose_subscriber/` (implemented) — subscribes to every GOOSE Control Block on one IED via
  `ied_model` (`IedModel_getGooseSubscriptionTargets`, never re-parses SCL, never discovers
  GoCBs over the wire), applying `GooseSubscriber_setDstMac`/`setAppId` filters from SCL's
  addressing when present, and delivers normalized `GooseSubscriberRecord`s via a
  caller-registered callback (JSON stringification deferred to `ipc_dispatcher`, same as
  `mms_report_client`). Unlike MMS, GOOSE never carries a reference on the wire at all, so
  `GooseSubscriberEntry.reference` is always resolved via a per-target cache of
  `IedModel_getDataSetMemberReferences` results (built once at `GooseSubscription_start`) —
  no server-truth branch exists the way it does for `mms_report_client`. Works under every
  `ied_model` `AccessMode`, including `REPORT_ONLY`.
  Public boundary: `src/features/goose_subscriber/service/goose_subscriber_api.h` — note the
  public functions are named `GooseSubscription_*`, not `GooseSubscriber_*`, deliberately:
  libiec61850's own `GooseSubscriber_create`/`GooseSubscriber_destroy` (`goose_subscriber.h`)
  would otherwise collide (identical names, different signatures) with this feature's public
  API of the same shape. GOOSE is connectionless (no MMS/TCP association), so there is no
  reconnect-supervisor thread the way `mms_report_client` has one — instead there's a single
  low-rate **liveness-polling** thread (see the "Hard Rules" exception below) that watches
  `GooseSubscriber_isValid()` per target and reports `VALID`/`STALE`/`INVALID_STATE`
  transitions via an optional status callback. Ethernet interface name is caller-supplied (no
  interface-name parsing in SCL, matching `mms_report_client`'s host/port convention). Proven
  end-to-end against a real `ied_simulator` IED publishing real GOOSE frames over `lo` in
  `integration_tests/goose_subscriber/` (requires `sudo` — see Commands). **`GooseSubscriberEntry`
  also now carries `previousValue`**, mirroring `MmsReportEntry`'s identical addition
  exactly (see that feature's own bullet above and "IPC / Reporting Out" below) —
  `shouldForwardAndUpdateCache` here has no `ReasonForInclusion` concept at all, so the same
  "cached==NULL → bootstrap, seed but don't forward" rule is GOOSE's entire mechanism for
  suppressing both the first-ever frame per target and the first frame after a liveness recovery.
- `src/orchestration/` (implemented) — sequences all five features above for **one IED**:
  `ipc_dispatcher` (bind + start its websocket service thread — first, deliberately, so a bind
  failure fails fast before touching the network-facing MMS/GOOSE side at all) -> `scl_bootstrap`
  (probe a host list, fetch SCL bytes) -> stage those bytes to a temp file -> (if `iedName` was
  empty: auto-detect it via `IedModel_listIedNames` on the staged file — exactly one `<IED>`
  found is used silently, zero or more than one fails hard with
  `ORCHESTRATION_ERR_IED_NAME_RESOLUTION_FAILED` at stage `ORCHESTRATION_STAGE_IED_NAME_RESOLUTION`,
  no interactive retry — an accepted limitation, every fixture in this repo has exactly one
  `<IED>` anyway) -> `ied_model` (load from that file) -> `mms_report_client` (start against the
  winning candidate's own host/port —
  not a separately supplied parameter, since bootstrap and reporting target the same physical
  IED; its report callback is unconditionally wired to `IpcDispatcher_onMmsReport`) ->
  `goose_subscriber` (start on a caller-supplied interface; same unconditional
  `IpcDispatcher_onGooseRecord` wiring). Public boundary:
  `src/orchestration/service/orchestration_api.h`. A top-level sibling of `src/features/`, not
  itself a "feature" in the Expected-features sense above — its whole job is sequencing
  already-implemented features, not talking to libiec61850/libwebsockets/cJSON directly (zero
  direct third-party includes in its own domain layer). `Orchestration_run` is one blocking call
  on the caller's thread (no orchestration-owned thread — `scl_bootstrap` is already synchronous,
  and `mms_report_client`/`goose_subscriber`/`ipc_dispatcher`'s own `_start()` calls each return
  synchronously once their background thread is launched). The staged temp file is unlinked
  immediately after `ied_model` loads it (fully parsed into memory in one call, so it has zero
  purpose afterward) rather than deferred. Fail-hard: if any stage fails, everything started by
  earlier stages in that same `Orchestration_run` call is torn down (reverse order) before
  returning, leaving the handle re-runnable — including tearing down an already-started
  `ipc_dispatcher`/`mms_report_client` if a later stage fails, no degraded partial-success mode.
  `Orchestration_stop`/`_destroy` tear down in the same reverse order (goose -> report client ->
  ipc_dispatcher -> ied_model release — ipc_dispatcher stops only after both producers are torn
  down, guaranteeing no more producer-thread calls can land on it once it stops). Single-IED
  scope only; no multi-IED support. `src/main.c` is now wired to this (see Current State above),
  and never reaches into `ipc_dispatcher` directly (see that feature's own bullet). Proven
  end-to-end against a real `ied_simulator` IED (MMS file services + buffered reports + GOOSE
  simultaneously, plus a real websocket client observing orchestration's own `ipc_dispatcher`
  output) in `integration_tests/orchestration/` (requires `sudo` — see Commands).
  **`Orchestration_runFromLocalFile(handle, sclFilePath, host, mmsPort, iedName, interfaceId,
  accessMode, outDetail)`** is a second public entry point for devices whose MMS server doesn't
  implement file services, so `scl_bootstrap` can never succeed against them even though
  they're a real, connectable MMS/GOOSE device (encountered in practice with OMICRON IED
  Scout's "Simulate IED" mode — see `main.c`'s own `sclFilePath` argv comment). Shares its
  entire continuation (IED-name auto-detect, `ied_model` load, `mms_report_client`/
  `goose_subscriber` start, same fail-hard rollback) with `Orchestration_run` via a private
  `runFromSclFile` helper in `orchestration_api.c` — the only difference is stage 0-1:
  `Orchestration_run` runs `ipc_dispatcher` start then `scl_bootstrap`+staging to produce a
  temp SCL path it owns (unlinked once `ied_model` has parsed it); `_runFromLocalFile` runs
  only `ipc_dispatcher` start, then hands the caller's own `sclFilePath` straight to that same
  continuation, unmodified and never deleted (not this handle's file to own). `host`/`mmsPort`
  are still required, direct parameters here (no bootstrap step to derive them from a scanned
  candidate) and still drive the real live `mms_report_client`/`goose_subscriber` connections —
  loading the model locally only changes *how the SCL description is obtained*, not the live
  MMS/GOOSE connection. Argument-validation-only unit coverage in
  `tests/orchestration/test_orchestration_api.c` (mirrors `Orchestration_run`'s own cases); no
  dedicated E2E test yet — exercised so far via manual verification against a real IED Scout
  simulation.
  **`Orchestration_runFromOnlineDiscovery(handle, host, mmsPort, iedName, interfaceId, accessMode,
  acseAuthPassword, outDetail)`** is a THIRD public entry point, for devices whose MMS server
  doesn't implement file services at all (unlike `_runFromLocalFile`'s scenario, there's no local
  SCL file to hand in either — a real OMICRON IED Scout "Simulate IED" instance confirmed to
  return `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` is the motivating case). Instead of parsing
  any SCL at all, it calls `ied_model_online_loader`'s `IedModelOnlineLoader_build` (own one-shot
  `IedConnection`, own ACSE-auth handling, own connect/discover/disconnect — this layer never
  touches `IedConnection` directly, preserving its "zero direct third-party includes" invariant)
  to walk the live device's MMS ACSI directory services and reconstruct an equivalent model, then
  joins the SAME shared tail `Orchestration_run`/`_runFromLocalFile` already use (`mms_report_client`/
  `goose_subscriber` start, fail-hard rollback) via a new private `runFromIedModelHandle` helper —
  `runFromSclFile` was refactored to call this same helper instead of duplicating that tail
  itself. New `ORCHESTRATION_STAGE_ONLINE_DISCOVERY` stage (replaces `BOOTSTRAP`/`STAGING`/
  `MODEL_LOAD` for this entry point only) and `ORCHESTRATION_ERR_ONLINE_DISCOVERY_FAILED`/
  `OrchestrationErrorDetail.onlineDiscoveryError`. `iedName` here only labels the constructed
  model (no SCL `<IED>` list exists to auto-detect from over a live connection) — NOT a
  resolution stage of its own, unlike the other two entry points' `iedName` semantics. **Never**
  invoked automatically inside `Orchestration_run` itself (that would silently change its
  behavior/latency based on how bootstrap happened to fail, violating its existing fail-hard,
  no-silent-branching contract) — `src/main.c` calls it explicitly, only as a one-shot retry after
  `Orchestration_run` itself has already failed with exactly that bootstrap status (see `main.c`'s
  own top comment). See `ied_model_online_loader/`'s own bullet below for the discovery mechanism,
  and the "No over-the-wire tree discovery" Hard Rule for why this is a narrow, explicit exception.
- `ipc_dispatcher/` (implemented) — relays normalized `MmsReportRecord`/`GooseSubscriberRecord`
  data out over a websocket, hosted internally only (`127.0.0.1`, no TLS, no
  `IpcDispatcherConfig` field even exists to bind elsewhere), for a separate API layer/frontend
  to consume. Push-only (no client-sent message handling at all — the vhost's protocol callback
  never handles `LWS_CALLBACK_RECEIVE`). Public boundary:
  `src/features/ipc_dispatcher/service/ipc_dispatcher_api.h`. Its two callback-adapter functions
  (`IpcDispatcher_onMmsReport`/`_onGooseRecord`) match `MmsReportClientCallback`/
  `GooseSubscriberCallback`'s signatures exactly and are registered **by `src/orchestration/`
  itself, unconditionally** — there is deliberately no `Orchestration_setReportCallback`/
  `_setGooseRecordCallback` for `src/main.c` (or any other caller) to reach around this with;
  `ipc_dispatcher`'s entire lifecycle (create/start/stop/destroy) is owned by
  `OrchestrationHandle` end to end, the same way `iedModel`/`reportClient`/`gooseSubscriber`
  already are — `main.c` only ever configures it via `OrchestrationConfig.ipcDispatcherConfig`
  and never includes this feature's own service header. Each adapter always calls the matching
  `_destroyReportRecord`/`_destroyRecord` before returning — ownership transfers to whichever
  single consumer is registered, which is exactly why there's no way to *also* register a
  second, caller-supplied observer for report/GOOSE data records (two consumers can't both
  own+destroy the same record; the three connection-state/RCB-status/liveness-diagnostic
  callbacks are untouched and remain directly caller-settable via orchestration, unrelated to
  ipc_dispatcher). Quality (`q`) pairing — flagged as unbuilt in "IPC / Reporting Out" below for
  a long time — is
  now solved here: `domain/ipc_dispatcher_usecases.c`'s `IpcDispatcherUseCases_pairQuality`
  (reference format confirmed via `IedModelUseCases_getDataSetMemberReferences`:
  `"<LDName>/<LN>$<FC>$<DO>$<DA>"`) finds each value's `q` sibling by walking **up its own
  ancestor prefixes** (one `$`-segment at a time via `findQualityIndexForValue`), not just
  stripping the last `$` — a flat attribute (`Pos$stVal`) finds its `q` one level up on the first
  try, same as a plain last-`$` strip; a deeply nested CONSTRUCTED-DA chain (a CMV's
  `PhV$phsA$cVal$mag$f`) walks past its own intermediate levels to find `PhV$phsA$q` several
  segments up instead — quality belongs to the whole CDC instance, not to whichever BDA happens
  to be the terminal leaf of one nested DA within it. The original single-`$`-strip
  implementation only ever found quality for flat attributes, silently leaving every nested
  measured value (most real MX/CMV datapoints) with `quality: null` — confirmed against real
  production traffic and fixed. Merges a matched `q` sibling into its value entry's one JSON data
  point (a lone `q` with no value sibling is dropped, not fabricated into a value-less point). Quality validity is decoded via `Quality_fromMmsValue`/`Quality_getValidity`
  (`iec61850_common.h`) into a named 4-value enum; the remaining detail/test/substituted/derived
  bits are copied verbatim into one raw `uint16_t` passthrough field rather than individually
  named in v1. `MmsValue` scalars are converted to JSON-friendly types by `MmsValue_getType()`
  (`utils/ipc_dispatcher_value_codec.c`) — boolean/integer/unsigned/float/string/UTC-time map
  directly; **`MMS_BIT_STRING` maps to a raw unsigned integer** (`MmsValue_getBitStringAsInteger`)
  — covers CODEDENUM-typed value DAs (`Dbpos`/`Tcmd`, per `IedModelUtils_mapBType`, e.g. a
  breaker's `Pos.stVal`) that wire-encode as a bitstring and previously fell through to the
  unsupported placeholder (found against real production traffic). Deliberately a raw integer,
  not a named enum string (e.g. Dbpos's own `0`=intermediate-state/`1`=off/`2`=on/`3`=bad-state
  per IEC 61850-7-3) — this function, by itself, has no way to know which specific CODEDENUM a
  given bitstring represents (Dbpos and Tcmd share the same wire type but different meanings), and
  guessing a decoded label without per-type verification would violate this repo's own "don't
  guess IEC 61850 semantics" rule; the raw bit pattern is always correct regardless. Quality's
  own bitstring never reaches this path at all — `IpcDispatcherUseCases_pairQuality` excludes
  every `q`-named entry from ever being treated as a value, routing it to `_decodeQuality`
  instead (see above), so this addition can't double-decode or conflict with quality handling.
  **This codebase previously carried a descriptive-label feature for the specific `Dbpos` case
  (an SCL-derived semantic side table plus an additive `"label"`/`"previousLabel"` JSON field),
  removed again at explicit user request** so every CODEDENUM value (`Dbpos`, `Tcmd`, or any
  other) is reported identically — a raw integer via `MmsValue_getBitStringAsInteger`, no
  per-type special casing anywhere in `ied_model`/`mms_report_client`/`goose_subscriber`/
  `ipc_dispatcher`. Removal was driven by the same reasoning the original addition's own caveat
  already flagged: treating one CODEDENUM subtype differently from the rest was itself the
  discrepancy the user wanted gone, not a value worth keeping despite it.
  Anything else (structures, arrays, octet strings, etc. — not reachable from today's
  leaf-DA-only FCDA datasets) falls back to an owned `"<unsupported:...>"` placeholder string,
  never silently dropped. **Threading is the crux of this feature**: `mms_report_client`'s
  reconnect-supervisor thread and `goose_subscriber`'s `GooseReceiver` reception thread each
  call an adapter directly and must never block, but libwebsockets requires all `lws_write`/
  context access to happen on the one thread running `lws_service()` — so each adapter only
  ever (a) serializes to JSON and (b) pushes it onto a bounded, mutex-guarded broadcast ring
  (`data/ipc_dispatcher_ring_buffer.c`, `hal_thread.h`'s `Semaphore` as a binary mutex, same
  primitive `goose_subscriber` already uses) then (c) calls `lws_cancel_service` — the *only*
  libwebsockets call ever made from a producer thread. `ipc_dispatcher`'s own dedicated
  service-loop thread (`data/ipc_dispatcher_ws_server.c`, `hal_thread.h`'s `Thread`, same pattern
  as `goose_subscriber`'s liveness thread) drains the ring per-connection via a read cursor;
  a lagging/slow client's own cursor jumps forward and drops its own unseen messages
  (start-from-now on connect, no backlog replay) — this cost is strictly per-connection and
  never feeds back into the MMS/GOOSE producer threads. `lws_service`'s 1000ms loop timeout is a
  bounded safety net, not a data-driving poll (real wakeups come from `lws_cancel_service`) —
  same class of narrow "no cyclic polling" exception as `goose_subscriber`'s liveness thread.
  `IpcDispatcher_stop` fully tears down the lws context (not just the service thread) so a
  subsequent `IpcDispatcher_start` on the same handle can cleanly rebind the same port — unlike
  `GooseReceiver`, an lws context's listening socket has no "stop servicing, keep the bind" mode.
  JSON envelope shape (stable contract — see "IPC / Reporting Out" below for the full example
  and field-by-field notes): `{schemaVersion, type: "MMS_REPORT"|"GOOSE", source: {...},
  hasTimestamp, timestampMs?, dataPoints: [{reference, value, quality, previousValue,
  previousQuality, label, previousLabel}]}` — `previousValue`/`previousQuality`/`label`/
  `previousLabel` are always present (`null` when not applicable), matching `quality`'s own
  convention; `dataPoints` itself now only ever contains points that actually changed (see "IPC /
  Reporting Out" below for the full change-stream rework).
  Proven end-to-end (real bind, a hand-rolled minimal RFC6455 test client — deliberately **not**
  libwebsockets client mode, so a bug shared by both ends of the same library can't hide from
  the test, same philosophy `integration_tests/ied_simulator/` already applies at the protocol
  level — real JSON delivery for both a synthetic MMS report and a synthetic GOOSE record) in
  `integration_tests/ipc_dispatcher/` — no `sudo`, no `ied_simulator` needed (loopback TCP only,
  and records are hand-built rather than sourced from a real IED, since this feature's job
  starts after `mms_report_client`/`goose_subscriber` have already normalized one).
- `ied_discovery/` (implemented) — a later, deliberate, user-requested addition (see
  Expected-features note above): finds candidate IEC 61850 MMS devices on the local network
  instead of requiring the operator to already know a target IP. Unlike every feature above,
  it is **not** part of `orchestration`'s own sequence — `src/main.c` calls it directly, before
  `Orchestration_run` even starts, only when the `host` argv is omitted/empty (see Current
  State above), and hands the one host it picks into the same, completely unmodified
  `Orchestration_run` call an explicitly-typed host would take. Two-stage verification per
  candidate: (1) a cheap bounded-concurrency TCP probe on the MMS port, reusing
  `scl_bootstrap`'s own async-probe machinery via its newly-exposed `SclBootstrap_tcpProbeOnly`
  (see that feature's own bullet); (2) for TCP survivors only, a real MMS/ACSE association
  (`IedConnection_connect`, immediately closed — no file browsing, no SCL fetch, that's deferred
  to `scl_bootstrap`'s own later, real run against whichever host is picked). Only a host that
  passes *both* stages counts as a confirmed device — this is deliberately host discovery on an
  already-named local interface (`getifaddrs()` + CIDR math, entirely net-new in this repo), not
  the Hard Rules' "no over-the-wire tree discovery" (that rule is about not walking a connected
  device's data-model tree over MMS before SCL is loaded; this never touches SCL/the data model
  at all). Public boundary: `src/features/ied_discovery/service/ied_discovery_api.h` — pure
  request/response, zero I/O prompting: `IedDiscovery_scanSubnet` (enumerate + verify every host
  on a named interface's subnet, minus its own address, capped by `IedDiscoveryConfig.maxHosts`
  as a safety valve against an accidentally huge range) and `IedDiscovery_verifyHost` (verify one
  caller-supplied host identically — used for manually-typed candidates, no special-cased
  trust). Optionally gated behind ACSE password auth (one retry, mirroring `scl_bootstrap`'s own
  policy) via its own independent `IedDiscoveryConfig.acseAuthPassword` — features never share
  config structs across the public boundary. This feature deliberately has **no interactive
  medium of its own** — it's driven by `scan_orchestration`'s worker (which calls
  `IedDiscovery_scanSubnet` once per sweep), which is in turn driven purely by
  `control_dispatcher`'s `START_SCAN`/`STOP_SCAN` commands now — an in-process terminal adapter
  (`src/main_discovery_prompt.c`) used to sit in front of this for a boot-time CLI flow but was
  removed once the control websocket covered the same ground (see this file's own `main.c`
  Current-State bullet). Proven end-to-end (`verifyHost`
  against a real `ied_simulator` IED, including the ACSE-auth-retry symmetry) in
  `integration_tests/ied_discovery/` — no `sudo` needed. A real subnet scan itself
  (`scanSubnet`/`getifaddrs`) is machine-topology-dependent and not automatable; see that E2E
  test's own Commands bullet for the one deterministic `getifaddrs` case that is covered, and
  for how to manually verify a real scan.
- `ied_model_online_loader/` (implemented) — a later, deliberate, user-requested addition (see
  Expected-features note above): builds a complete `IedModelHandle` by walking a live IED's own
  MMS ACSI directory/model-discovery services, for devices whose MMS server associates fine but
  never serves an SCL file over file services at all (confirmed in practice against a real
  OMICRON IED Scout "Simulate IED" instance — `scl_bootstrap` returns
  `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` even after browsing its file directory). This is the
  **one** narrow, deliberate exception to the "No over-the-wire tree discovery" Hard Rule below —
  every call this feature makes is itself a `libiec61850` client API call (`IedConnection_get*`),
  never hand-rolled protocol parsing, and it only ever runs as an explicit, caller-invoked
  fallback (`Orchestration_runFromOnlineDiscovery`), never silently instead of SCL parsing. Public
  boundary: `src/features/ied_model_online_loader/service/ied_model_online_loader_api.h`, one
  entry point: `IedModelOnlineLoader_build(host, port, iedName, mode, acseAuthPassword, config,
  outError)` — owns its own one-shot `IedConnection` end-to-end (create, optional ACSE password
  auth via its own `data/ied_model_online_loader_auth.c`, duplicated from `scl_bootstrap`/
  `mms_report_client`'s identical snippet per this codebase's own no-cross-feature-data-layer-reuse
  convention, connect, discover, close+destroy) — same "owns its own MMS session" shape as
  `SclBootstrap_scanAndFetch`/`MmsReportClient_create`, never a caller-supplied connection.
  Discovery sequence, all via `libiec61850`'s `IEC61850_CLIENT_MODEL_DISCOVERY` API group:
  `IedConnection_getLogicalDeviceList` (LDs) -> `getLogicalDeviceDirectory` (LNs per LD) ->
  per-LN, `getLogicalNodeDirectory` with `ACSI_CLASS_BRCB`/`ACSI_CLASS_URCB`/`ACSI_CLASS_GoCB` to
  enumerate control blocks directly (no FC-naming-convention guessing — the server states each
  block's class outright) and `ACSI_CLASS_DATA_OBJECT` + `getDataDirectoryByFC` at FC=ST/MX (the
  same "reportable" FC pair `IedModel_getReadTargets`/`_getReportableAttributeReferencesForLogicalNode`
  already treat as the whole story) to build the DO/DA tree, with `getVariableSpecification`
  resolving each leaf's coarse MMS type -> per-RCB `IedConnection_getRCBValues` and per-GoCB
  `IedConnection_getGoCBValues` to read `DatSet`/`ConfRev`/(for GoCB) `DstAddress`, then
  `IedConnection_getDataSetDirectory` to resolve any non-empty dataset's ordered member list -
  the live-wire equivalent of `IedModel_getDataSetMemberReferences`'s own SCL-derived output.
  Builds a real dynamic `IedModel*` from all of this (`IedModel_create`/`LogicalDevice_create`/
  `LogicalNode_create`/`DataObject_create`/`DataAttribute_create`/`DataSet_create`/
  `DataSetEntry_create`/`ReportControlBlock_create`/`GSEControlBlock_create` — the same
  dynamic-model construction calls `integration_tests/ied_simulator/src/sim_server.c` already uses
  and the same two documented gotchas apply: no LD-prefix on `DataSetEntry_create`'s reference,
  bare local name for `DataSet_create`), then hands it to `ied_model`'s new
  `IedModel_wrapDynamicModel(model, iedName, mode)` constructor — the one new function `ied_model`
  itself gained for this (a thin `sIedModelHandle` wrap, symmetric to `IedModel_loadFromFile`'s
  own), so every existing accessor (`getReportSubscriptionTargets`, `getGooseSubscriptionTargets`,
  `getDataSetMemberReferences`, `getReportableAttributeReferencesForLogicalNode`) behaves
  identically regardless of whether the model came from SCL parsing or live discovery —
  `mms_report_client`/`goose_subscriber` require zero changes to consume it, including their
  existing dynamic-("Dyn"-dataset) handling for a discovered RCB/GoCB with no configured dataset
  (this loader just leaves `datasetReference` NULL in that case, exactly like an SCL-parsed
  `datSet="Dyn"` RCB already does — no new logic needed, `mms_report_client`'s existing
  `getOrCreateDynamicDataset` already covers it).
  Reference-format bridging: `domain/ied_model_online_loader_usecases.c`'s
  `IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef` converts `getDataSetDirectory`'s ACSI
  dot/bracket-form references (`"LD/LN.DO[.SDO...].DA[FC]"`) into this codebase's own `"$"`-joined
  wire form (`"LD/LN$FC$DO[$SDO...]$DA"`) — the exact mirror image of `mms_report_client`'s
  existing `MmsReportClientUseCases_buildWireMemberReferences` (which converts the same wire form
  the OTHER direction, for `IedConnection_createDataSet`). Array-index annotations
  (`"item(idx)component"`) are stripped rather than preserved — this codebase doesn't model array
  indices anywhere else either (see `ied_model`'s own documented, deliberately deferred `DAI/@ix`
  limitation).
  **Known, deliberately accepted v1 limitations** (mirroring how `mms_report_client`'s own
  dynamic-dataset work documents its limitations rather than glossing over them): only builds
  FC=ST/MX structure, so `IedModel_getReadTargets`/`_getControlTargets` see an incomplete/empty
  tree against a discovered model — accepted since this entry point only ever drives
  report/GOOSE consumption; `DataAttributeType` is only a coarse mapping from the MMS wire type
  (confirmed harmless today — no existing `ied_model` accessor ever reads `DataAttributeType`
  back off a built node, only `ModelNode_getType()`/FC are consulted); GoCB addressing
  (`DstAddress`'s VLAN/AppID/MAC) is treated as "populated" only if not all-zero, since libiec61850
  exposes no sentinel distinguishing "server never set this optional attribute" from "explicitly
  zero" — unverified against a real device, degrades safely to unfiltered-by-`gocbRef`-only
  reception either way (`goose_subscriber` already handles `hasAddress==false` with zero
  special-casing); no dataset-count/`maxAttributes`-cap handling, same accepted limitation
  `mms_report_client`'s own dynamic-dataset creation already has; discovery is materially slower
  and more MMS-request-heavy than one SCL file transfer + local parse, scaling with model size —
  the exact tradeoff the Hard Rule below already warns about, accepted here only because it's the
  sole way to report anything from a file-service-less device at all.
  **A third dynamic-model construction gotcha, found the hard way while building this
  feature's own E2E test** (alongside the two already documented from `ied_simulator`):
  `LogicalDevice_create(name, parent)` implicitly PREPENDS its parent `IedModel`'s own name to
  `name` to form the LD's real wire name — feeding it `IedConnection_getLogicalDeviceList`'s
  already-fully-qualified names (e.g. `"Reporter1LD1"`) against a model built via
  `IedModel_create("Reporter1")` produced a corrupted double-prefixed name,
  `"Reporter1Reporter1LD1"`. Fixed by building the internal model via `IedModel_create("")` —
  sidesteps ever needing to know/derive the server's true IED name at all, since every LD name
  discovery ever handles is already fully qualified straight from the wire. Relatedly:
  `ReportControlBlock_create`'s `dataSetName` and `GSEControlBlock_create`'s `dataSet` parameters
  both want the BARE local dataset name (confirmed against `ied_model_scl_loader.c`'s own usage,
  which always passes SCL's raw `datSet="..."` attribute straight through) — passing
  `getRCBValues`/`getGoCBValues`'s own fully-qualified reference there instead (as this loader
  did before the E2E test caught it) produces an unresolvable double-qualified reference once
  `IedModelUseCases_getReportSubscriptionTargets`/`_getGooseSubscriptionTargets` re-prepend their
  own `lnRef$` on top of it a second time.
  Proven end-to-end against a real `ied_simulator` IED with its MMS file services pointed at a
  real, empty fixture directory (`SimServer_setFilestoreBasepath` + `fixtures/no_scl_files/`,
  mirroring `scl_bootstrap`'s own identical fixture/precedent) in
  `integration_tests/ied_model_online_loader/` — no `sudo` needed (MMS/TCP only). Deliberately
  **not** simulated via a file-services-disabled server config: that was tried first and
  empirically produces a different `scl_bootstrap` outcome
  (`SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED`, not `NO_SCL_FILE_FOUND`) than what a real OMICRON
  IED Scout instance actually returns, so it would have tested the wrong precondition. Manually
  verify GoCB reference-form/`DstAddress` behavior against a real OMICRON IED Scout instance
  before relying on it in production, per this bullet's own flagged unknowns.
  **A fourth bug, found against real production hardware (a device whose MMS server never serves
  an SCL file, forcing it through this loader): every MMS report from such a device showed EVERY
  data point as `"<unsupported:structure>"` with `quality: null`, while GOOSE on the same device
  worked fine.** Root cause: `IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef`
  (`domain/ied_model_online_loader_usecases.c`) built its output using the ACSI reference's whole
  `"LD/LN"` prefix, unsplit, as the first `$`-segment (e.g.
  `"VR4C1C01A1LD0/SP16GGIO5$ST$Ind"`) — handed straight to `DataSetEntry_create` as its
  `variableName`, directly violating this file's own documented dynamic-model gotcha #1 (no
  LD-wire-name prefix) that the call site's own comment claimed was already satisfied but wasn't.
  Consequence: `IedModelUseCases_getDataSetMemberLeafReferences` (`ied_model`, shared by both
  `mms_report_client`/`goose_subscriber`) tokenizes `entry->variableName` on `$` and passes the
  first token straight to `LogicalDevice_getLogicalNode` as a bare LN name — for every
  online-discovered member this token was actually `"LD/LN"` combined, so the LN lookup silently
  failed, `IedModel_getDataSetMemberLeafReferences` returned an empty (not NULL) list — outwardly
  indistinguishable from "already a leaf, nothing to decompose" — Gap-4 structure decomposition
  was never attempted for any online-discovered DO-level member, and the raw, undecomposed
  `MMS_STRUCTURE` reached `ipc_dispatcher`'s value codec, which has no branch for it. GOOSE wasn't
  affected on this particular device only because its own GOOSE dataset(s) happen to be
  leaf-level already (or don't include this structured DO) — it shares the exact same buggy
  conversion and would hit the identical failure for any DO-level GOOSE dataset member. Fixed by
  splitting the `"LD/LN"` prefix on `/` inside `convertAcsiRefToWireRef` and using only the LN
  portion in the output, matching the SCL loader's own convention exactly; the misleading "No
  LD-name prefix" comment at the `DataSetEntry_create` call site
  (`data/ied_model_online_loader_connection.c`) was corrected to describe what the code actually
  does now instead of asserting something false. Proven via a new
  `tests/ied_model_online_loader/test_ied_model_online_loader_usecases.c` (a direct regression
  test: a DO-level and a leaf-level ACSI reference each now convert to a bare-LN-prefixed wire
  reference, plus the pre-existing malformed-input/NULL cases) — no existing E2E fixture exercises
  Gap-4 decomposition through this loader (`integration_tests/ied_model_online_loader/`'s own
  assertions stop at report/GOOSE target-list shape, not per-report leaf decomposition), so this
  is unit-level-only coverage for now.
- `scan_dispatcher/` (implemented) — an eighth feature, a later, deliberate, user-requested
  addition (see Expected-features note above): a near-verbatim structural duplicate of
  `ipc_dispatcher`'s ring-buffer + libwebsockets-service-thread transport (own port, default
  **8766** — distinct from `ipc_dispatcher`'s 8765 since both can be bound in the same process),
  relaying "device found" scan events instead of MMS-report/GOOSE data. Deliberately
  **duplicated, not shared** — `ipc_dispatcher`'s ring buffer/ws-server code is already
  content-agnostic (pushes/reads opaque JSON strings, zero MMS/GOOSE coupling), but this
  codebase has no precedent for a cross-feature "shared" directory, and every other reusable
  third-party-integration snippet in this repo (e.g. ACSE-auth setup) is already duplicated per
  feature rather than factored out, for the same "features never reach into each other's
  data/domain layers" reasoning. Public boundary:
  `src/features/scan_dispatcher/service/scan_dispatcher_api.h` — create/start/stop/destroy
  mirror `ipc_dispatcher`'s own contract exactly (stop fully tears down the lws context so a
  later start cleanly rebinds), plus one **typed** publish entry point,
  `ScanDispatcher_publishDeviceFound(handle, scanId, host, mmsPort)` — deliberately not a
  generic `publishJson`, since `ipc_dispatcher`'s own public surface has zero such entry point
  either (`_onMmsReport`/`_onGooseRecord` both take typed structs) and keeping the envelope's
  field-naming knowledge entirely inside this feature preserves the "message shape is a stable
  contract" single-source-of-truth property. This feature has **no knowledge of scans,
  interfaces, or reference-counting at all** — purely transport; `scan_orchestration` (below)
  decides when to start/stop it. JSON envelope (stable contract): `{schemaVersion, type:
  "SCAN_RESULT", scanId, host, mmsPort, discoveredAtMs}` — `discoveredAtMs` sourced from
  `Hal_getTimeInMs()` (wall-clock ms since epoch, same HAL call `scl_bootstrap_tcp_probe.c`
  already uses). Proven end-to-end (real bind, the same hand-rolled minimal RFC6455 test-client
  shape `ipc_dispatcher`'s own E2E test uses, hand-built `publishDeviceFound` calls rather than a
  real scan — this feature's job starts after a scan has already found something) in
  `integration_tests/scan_dispatcher/` — no `sudo`, no `ied_simulator` needed.
- `src/scan_orchestration/` (implemented) — a top-level sibling of `src/features/` (not itself a
  "feature" in the Expected-features sense, same as `src/orchestration/`), sequencing
  `ied_discovery` (subnet enumeration + host verification, left entirely **untouched** — this
  layer only ever calls its existing, unmodified `IedDiscovery_scanSubnet`/`_create`/`_destroy`)
  and `scan_dispatcher` (transport, above) into a continuous, background, reference-counted,
  multi-scan-capable service. Public boundary:
  `src/scan_orchestration/service/scan_orchestration_api.h` —
  `ScanOrchestration_create`/`_destroy`, `_setDeviceFoundCallback` (one process-wide slot,
  snapshotted per-worker at start time, mirrors `Orchestration`'s own single-callback-slot
  convention), `ScanOrchestration_startScan(handle, request, outScanId)` /
  `_stopScan(handle, scanId)` (returns a `uint64_t scanId`, a simple mutex-guarded monotonic
  counter starting at 1 — no string-alloc churn, and the registry already needs a mutex for the
  active-scan array anyway), and `_snapshotDiscoveredHosts` (a thread-safe read-only snapshot of
  one scan's currently-announced hosts, for a LOCAL, in-process caller that needs the live,
  growing list without connecting to the daemon's own websocket as a client of itself — no such
  in-process caller exists today since `main_discovery_prompt.c` was removed, but the function
  stays as public API; every current caller instead gets scan results over `scan_dispatcher`'s
  own websocket, same as any other client).
  **Per-scan worker** (`data/scan_orchestration_worker.c`): owns a **private**
  `IedDiscoveryHandle` and its own mutex-guarded seen-set of already-announced hosts, looping
  sweep (`IedDiscovery_scanSubnet`, reused as one blocking call per sweep — deliberate
  simplification, results become visible at sweep-end rather than incrementally mid-sweep,
  avoids touching tested `ied_discovery` internals) -> diff each result against the seen-set ->
  publish only genuinely new hosts (to `scan_dispatcher` and the optional caller callback) ->
  interruptible sleep for the configured sweep interval -> repeat, until stopped. Mirrors
  `goose_subscriber_connection.c`'s `interruptibleSleep`/`stopRequested`/`exited` idiom exactly
  (`hal_thread.h` has no `Thread_join` — the worker sets `exited=true` as its last act, the stop
  function does a bounded `while(!exited) Thread_sleep(20)` loop). **Known, accepted
  limitation**: stop cannot interrupt an in-flight sweep — `IedDiscovery_scanSubnet` has no
  cancellation hook — so `ScanOrchestration_stopScan` blocks until the current sweep's own
  `scanSubnet` call returns on its own, worst case bounded by `tcpProbeTimeoutMs *
  ceil(hostCount/maxConcurrentTcpProbes) + mmsConnectTimeoutMs * tcpSurvivorCount` (several
  seconds for a /24 at default config) — `main.c` prints a "Stopping scan..." diagnostic before
  calling it, since this is a real, user-visible pause.
  **Registry** (`data/scan_orchestration_registry.c`) is the refcounting core, deliberately
  **two-phase locked**: a short critical section reserves scanIds and does the 0→1
  `ScanDispatcher_start`/registration together (a bind failure here registers nothing), but
  removal (`_remove`) only unregisters a worker under the lock and hands it back to the caller
  to stop/destroy — the potentially slow, blocking part — entirely **outside** any lock,
  afterward, along with a separately-decided 1→0 `ScanDispatcher_stop`. Without this split, one
  scan's slow stop (see the worker's own known limitation above) would serialize every other
  concurrent scan's start/stop behind it, directly violating "multiple concurrent scans must run
  independently."
  Proven end-to-end (real `ScanOrchestration_startScan`/`_stopScan` against the real `lo`
  interface with two different `mmsPort`s, a hand-rolled websocket test client probing the
  shared dispatcher's liveness through the full 0→1→2→1→0→1-rebind refcount cycle) in
  `integration_tests/scan_orchestration/` — no `sudo` needed. Sweeps against `lo` are expected
  to fail (see that test's own Commands bullet) — this test proves sequencing/refcounting/
  threading, not sweep success.
- `src/device_manager/` (implemented) — a top-level sibling of `src/features/`/
  `src/orchestration/`/`src/scan_orchestration/` (not itself a "feature" in the Expected-features
  sense), added at explicit user request to support reporting on **multiple IEDs at once**: runs
  SEVERAL `orchestration` pipelines concurrently, one per physical IED, each auto-assigned its
  own `ipc_dispatcher` websocket port, addressable by a server-generated `deviceId`. Confirmed via
  codebase research before building this: the feature layer was ALREADY multi-instance-safe (no
  global/static state anywhere in `ipc_dispatcher`/`mms_report_client`/`goose_subscriber`/
  `orchestration`/`ied_model`/`scl_bootstrap` blocks running several concurrent
  `OrchestrationHandle`s in one process — every bit of state lives in a per-call handle struct;
  `GooseReceiver` opens its own independent raw socket per instance, so multiple on the same
  interface for different devices is fine, standard Linux behavior). What was actually missing,
  and what this layer plus `control_dispatcher/` (below) provide: a registry that can run several
  `OrchestrationHandle`s at once, and a control-plane transport to actually call start/stop from
  outside the process. A synchronous library, no thread of its own (same as
  `ScanOrchestration_startScan`/`_stopScan`) — `DeviceManager_startReporting`/`_stopReporting`
  each block the calling thread for as long as the underlying `Orchestration_run*`/`_stop` call
  takes, but never serialize behind a DIFFERENT device's own slow call. Public boundary:
  `src/device_manager/service/device_manager_api.h` —
  `DeviceManager_startReporting(handle, host, mmsPort, iedName, interfaceId, sclFilePath,
  acseAuthPassword, accessMode, outDeviceId, outWsPort, outDetail)` /
  `DeviceManager_stopReporting(handle, deviceId, outDetail)`. `iedName` is mandatory (validated,
  fails fast) whenever `sclFilePath` is given — a stricter, explicit contract this feature adds
  on top of `Orchestration_runFromLocalFile`'s own auto-detect, per the original request; optional/
  auto-detectable otherwise, matching `Orchestration_run`'s existing semantics.
  **`domain/device_manager_bootstrap_policy.c`** extracts `main.c`'s own original inline
  sequencing (`if sclFilePath: _runFromLocalFile; else: _run, then retry via
  _runFromOnlineDiscovery exactly once on SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND`) into one
  reusable function, so both `main.c`'s boot-time device and `control_dispatcher`'s worker thread
  share exactly one copy — this is also the answer to "if no file is given, discover the
  structure": that's `scl_bootstrap`/`ied_model_online_loader`'s existing job (see their own
  bullets), **not** `ied_discovery`'s (which only finds an unknown host IP on the LAN — a
  separate, already-solved problem via `scan_orchestration`).
  **`data/device_manager_port_allocator.c`**: a simple `[rangeStart, rangeEnd]` range (default
  `9000`-`9999`) with a free-list populated on release (reuse-after-stop preferred over growing
  the never-yet-issued counter, keeps the allocated range compact) — explicit bookkeeping, no
  bind-and-retry probing.
  **`data/device_manager_registry.c`** is the two-phase-locked core, a direct structural mirror of
  `scan_orchestration_registry.c` (short-lock reserve/remove) with one extra phase START needs
  that STOP/scan_orchestration's own registry doesn't: the slow step (`Orchestration_run*`) must
  happen *after* the deviceId+port are reserved (so two concurrent starts never race for the same
  port or duplicate a host) and *before* the reservation is finalized or rolled back — three
  phases total: `_reserve` (short lock: host-dedupe check, deviceId+port allocation, `running=false`
  placeholder insert) → `Orchestration_create`/`DeviceManagerBootstrapPolicy_run` (**no lock
  held** — the slow part, real network I/O, seconds-scale) → `_finalize`/`_rollback` (short lock).
  Stop mirrors `scan_orchestration`'s own two phases exactly: `_removeIfRunning` (short lock,
  atomic find-and-remove so a concurrent duplicate stop can never double-tear-down; returns
  `DEVICE_MANAGER_ERR_START_IN_PROGRESS` if the entry exists but is still mid-start on another
  thread — no cancellation hook exists, same accepted limitation `ScanOrchestrationWorker_stop`
  already documents for its own in-flight sweep) → `Orchestration_stop`/`_destroy` (**no lock
  held**) → `_freePort` (short lock).
  **`acseAuthPassword` is a real, easy-to-miss use-after-free risk, handled explicitly**:
  `OrchestrationConfig` documents its own `acseAuthPassword` fields as "stays borrowed... caller
  must keep it alive for the `OrchestrationHandle`'s whole lifetime" — true for `main.c`'s own
  argv (alive for the whole process), but NOT true once a value arrives via a parsed
  control-plane message, whose source buffer is freed the moment that one request finishes
  processing. `DeviceManagerRegistry_reserve` therefore `strdup`s it immediately into the
  registry entry and hands back a BORROWED pointer to its own copy for the caller to wire
  directly into `OrchestrationConfig` — the copy is freed only after `Orchestration_destroy` has
  actually run (rollback or stop), never before.
  **Host-duplicate-start policy** (default, not dictated by the original ask, trivially relaxed):
  a second `StartReporting` for the same `(host, mmsPort)` while one is already running or
  mid-start is rejected with `DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING`, rather than silently
  doubling MMS/GOOSE load on the same physical device for no benefit.
  Proven end-to-end (two real `ied_simulator` instances at two `mmsPort`s, two concurrent
  `DeviceManager_startReporting` calls from two threads, a coarse non-flaky wall-clock bound
  proving they didn't fully serialize, distinct deviceIds/ports each streaming real GOOSE JSON,
  stop-one-leaves-other-running, stop-second-frees-port-for-reuse) in
  `integration_tests/device_manager/` — needs `sudo` (GOOSE, inherited transitively).
  **`device_manager` does NOT watch connection health or auto-stop a device on connection loss** —
  a device is torn down ONLY by an explicit `STOP_REPORTING` call. A connection-health monitor
  (a reaper thread that auto-stopped a device on a genuine MMS disconnect or GOOSE staleness
  transition, broadcasting a `DEVICE_STOPPED` message) was added at one point, then removed again
  at explicit user request — reverted back to this simpler contract. On a real connection loss,
  `mms_report_client`/`goose_subscriber`'s own existing, unmodified, retry-forever internal
  supervisor loops (see their own Architecture bullets above) just keep reconnecting with
  exponential backoff, exactly as they did before that feature existed — there is no
  `device_manager`-level reaction to connection state at all, and `Orchestration_setReportConnStateCallback`/
  `_setGooseStatusCallback` (pre-existing, generic pass-through setters on `orchestration`'s own
  public API) have no caller anywhere in this codebase.
- `control_dispatcher/` (implemented) — a ninth feature, the first **bidirectional** websocket
  surface in this codebase: unlike `ipc_dispatcher`/`scan_dispatcher` (push-only, confirmed via
  grep — no `LWS_CALLBACK_RECEIVE` anywhere in either), this one RECEIVES JSON commands
  (`START_REPORTING`/`STOP_REPORTING`/`START_SCAN`/`STOP_SCAN`) over its one well-known websocket
  port (default **8767** — next after `ipc_dispatcher`'s 8765 and `scan_dispatcher`'s 8766; a
  single shared control channel, not per-device — only the per-device report websockets, from
  `ipc_dispatcher`, are per-device) and pushes back JSON acks/errors, relaying the first pair to
  `device_manager` and the second pair to `scan_orchestration`. Public boundary:
  `src/features/control_dispatcher/service/control_dispatcher_api.h` —
  `ControlDispatcher_create(config, deviceManager, scanOrchestration, outError)` (both handles
  borrowed — caller, `main.c`, owns their lifetime; both required non-NULL) / `_start` / `_stop` /
  `_destroy`, same create/start/stop/destroy contract as `ipc_dispatcher`/`scan_dispatcher`. This
  is the daemon's **only** interface now — `main.c` has no argv, no boot-time device, and no
  interactive prompt anymore (see this file's own `main.c` Commands/Current-State bullets).
  **This is the first `cJSON_Parse` call in this codebase's production code** —
  `data/control_dispatcher_json_parser.c` — directly correcting the cJSON vendoring bullet's
  former "push-only... no parse counterpart in production code" claim (see that bullet, now
  updated). Every field is defensively type/`NULL`-checked before use, since this is untrusted
  network input, unlike every other cJSON use in this repo (which only ever serializes trusted,
  internally-generated data).
  **Threading** (three threads total, one genuinely new in kind — the first callback in this
  codebase whose job is anything besides draining a broadcast ring): (1) the lws service thread
  (`data/control_dispatcher_ws_server.c`, adapted from `ipc_dispatcher`/`scan_dispatcher`'s own)
  handles `LWS_CALLBACK_RECEIVE` by accumulating fragments into a bounded (8KB) per-session
  buffer; on the final fragment, parses via `control_dispatcher_json_parser.h` — a parse/
  validation failure OR a full request queue is handled ENTIRELY on this thread (builds the error
  JSON directly, pushes it straight onto the ring buffer, triggers writable) and never touches
  the worker; a successfully parsed+queued request instead calls a generic
  `ControlDispatcherRequestQueuedCallback` (deliberately a function pointer, not a direct
  dependency on the worker's own type — `control_dispatcher_ws_server.h` never includes
  `control_dispatcher_worker.h`; the worker is the one thing depending on the ws-server, for
  `_wake()`, not the other way around, resolving what would otherwise be a circular reference).
  (2) The dedicated worker thread (`data/control_dispatcher_worker.c`, `Semaphore_create(0)` as a
  wake signal — the exact idiom `mms_report_client_connection.c`'s own supervisor thread already
  uses) pops the request queue and runs the one genuinely slow call
  (`DeviceManager_startReporting`/`_stopReporting` for the two reporting actions;
  `ScanOrchestration_startScan`/`_stopScan` for the two scan actions) off the lws thread entirely,
  then pushes the JSON result onto the ring buffer and calls `ControlDispatcherWsServer_wake` (the
  one libwebsockets call a producer thread may make directly, same rule `ipc_dispatcher`/
  `scan_dispatcher` already document). (3) the lws thread already counted in (1).
  **Request queue** (`data/control_dispatcher_request_queue.c`): bounded FIFO of `ControlRequest*`,
  its own `Semaphore(1)`-as-mutex, deliberately separate from the ring buffer's own lock (different
  producer/consumer thread pairs — queue is lws→worker, ring buffer is worker→lws). A full queue
  is `SERVER_BUSY`, built and returned directly from the lws thread — never blocks it.
  **JSON envelope** (stable contract): inbound
  `{requestId, action: "START_REPORTING"|"STOP_REPORTING"|"START_SCAN"|"STOP_SCAN", params: {...}}`
  — `START_REPORTING` params: `host` required non-empty, `mmsPort` optional default 102, `iedName`
  optional unless `sclFilePath` is given (then mandatory), `interfaceId` required non-empty (no
  `"eth0"` default — that only ever matched developer machines), `sclFilePath`/`acseAuthPassword`
  optional, `accessMode` optional one of `"REPORT_ONLY"`/`"READ_ONLY"`/`"READ_AND_WRITE"`, default
  `"REPORT_ONLY"` → success result `{deviceId, wsPort}`; `STOP_REPORTING` params: `deviceId`
  required non-negative integer → success result `{deviceId}`; `START_SCAN` params: `interfaceId`
  required non-empty, `mmsPort` optional default 102, `sweepIntervalMs` optional (`0`/omitted =
  `scan_orchestration`'s own default) — deliberately **no** `acseAuthPassword` field (control-
  triggered scans are always unauthenticated, `control_dispatcher_usecases.c`'s own comment
  explains why) → success result `{scanId}`; `STOP_SCAN` params: `scanId` required non-negative
  integer → success result `{scanId}`. Outbound (every action): `{schemaVersion, requestId,
  action, success, result, error}` — `error: {code, message, stage?, detail?}`. `code` is a stable
  string: for the two reporting actions, mirrors `DeviceManagerError` (`INVALID_ARGUMENT`,
  `HOST_ALREADY_RUNNING`, `ORCHESTRATION_FAILED`, `DEVICE_NOT_FOUND`, `START_IN_PROGRESS`,
  `PORT_EXHAUSTED`); for the two scan actions, mirrors `ScanOrchestrationError`
  (`INVALID_ARGUMENT`, `OUT_OF_MEMORY`, `DISPATCHER_START_FAILED`, `THREAD_CREATE_FAILED`,
  `DISCOVERY_CREATE_FAILED`, `SCAN_NOT_FOUND`); plus parse-side codes shared by all four actions
  (`MALFORMED_REQUEST`, `UNKNOWN_ACTION`, `SERVER_BUSY`); `stage`/`detail` populated only for
  `ORCHESTRATION_FAILED` (reporting actions only — the scan actions have no equivalent staged
  detail), via the public `OrchestrationUtils_stageToString`/`_candidateStatusToString` (extracted
  from `main.c`'s own original private helpers so both `main.c` and this feature share one copy
  instead of duplicating the string tables).
  **Fan-out is broadcast to every connected control client** (identical mechanism to
  `ipc_dispatcher`'s own broadcast, filterable client-side by `requestId`) — the simplest
  possible v1 design, reusing the proven ring buffer verbatim; flagged as a real tradeoff if the
  "loopback-only, one real client (the future external API layer)" trust assumption ever changes.
  Proven end-to-end (real bind, a hand-rolled RFC6455 client that both SENDS masked command
  frames — the first E2E test in this repo to send a client→server frame — and receives
  responses; malformed-JSON/unknown-action cases need no privilege/simulator at all; a real
  `START_REPORTING`/`STOP_REPORTING` round trip against a real `ied_simulator` instance proves
  the whole chain end to end; a real `START_SCAN`/`STOP_SCAN` round trip against a real
  `ScanOrchestrationHandle` over `lo` needs no `sudo`) in `integration_tests/control_dispatcher/`
  — needs `sudo` only for the `START_REPORTING`/`STOP_REPORTING` real-device case.
  **No unsolicited `DEVICE_STOPPED` push exists** — an earlier revision added one
  (`ControlDispatcher_notifyDeviceStopped`), wired to `device_manager`'s connection-health monitor;
  both were removed together at explicit user request (see `device_manager/`'s own Architecture
  bullet above) — a device only ever leaves the registry via an explicit `STOP_REPORTING`
  request/response round trip, which every connected client already gets its own direct ack for.

## The Two Workers
- **GOOSE Sniffer** — `GooseReceiver`/`GooseSubscriber` (see `third_party/include/goose_receiver.h`,
  `goose_subscriber.h`) via `GooseReceiver_start()`'s library-managed raw-socket reception
  thread — not libpcap/Npcap (libiec61850's own `hal_ethernet` PAL provides the raw
  AF_PACKET-style socket directly; no separate capture library is used or vendored). Event-driven
  on frame arrival, never polled for *reception*. Implemented in `src/features/goose_subscriber/`.
- **MMS Report Client** — `IedConnection` (see `iec61850_client.h` for the client-side `ClientReportControlBlock`/`ClientReport`/`ReportCallbackFunction` API; `reporting.h` is the *server*-side implementation, not what a client uses), driven entirely by BRCB/URCB. Never poll for data; BRCB/URCB pushes reports on the IED's configured triggering conditions. Implemented in `src/features/mms_report_client/`.
- Both workers normalize output to a common JSON report shape before it reaches IPC.

## Hard Rules (with reasons)
- **libiec61850 is mandatory** for all protocol handling — never hand-roll GOOSE or MMS parsing; this is a correctness and safety liability in this domain.
- **No cyclic polling**, on either worker — polling defeats the reporter's whole reason for existing (event-driven, low-latency). **One narrow, deliberate exception**: `goose_subscriber`'s liveness thread (`data/goose_subscriber_connection.c`) polls `GooseSubscriber_isValid()` at a low rate purely to detect *staleness* (a publisher going silent) — GOOSE is connectionless and has no push signal for that (unlike `IedConnection`'s state-changed handler). It never gates or delays actual record delivery, which stays 100% event-driven via `GooseListener`; it only detects the absence of frames, which is structurally unobservable any other way in a connectionless protocol.
- **No over-the-wire tree discovery.** Parse `.scd`/`.icd` at boot to know what to subscribe to — runtime discovery is slow and fragile against flaky IEDs. **One narrow, deliberate exception**: `ied_model_online_loader` walks a live server's MMS ACSI directory services (`GetLogicalDeviceList`/`GetLogicalDeviceDirectory`/`GetLogicalNodeDirectory`/`GetDataDirectory[ByFC]`/`GetDataSetDirectory`/`GetRCBValues`/`GetGoCBValues`) to reconstruct an equivalent `IedModel` purely as a **fallback**, engaged only via the explicit `Orchestration_runFromOnlineDiscovery` entry point when `scl_bootstrap` has already exhausted every candidate with `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` (a real, connectable IED — e.g. OMICRON IED Scout's "Simulate IED" mode — that never serves an SCL file over MMS file services at all). It never runs as a silent, automatic substitute inside `Orchestration_run` itself, and it's the same "no other way to observe this" class of exception as `goose_subscriber`'s liveness-polling thread above: without it, a device with no file services is entirely unreportable by this daemon, full stop. See `ied_model_online_loader/`'s own Architecture bullet for the full mechanism.
- **No dangling connections.** Explicit pooling, keep-alives, exponential backoff on the MMS side — IEDs drop connections under load.
- **Don't touch `third_party/`** — it's pre-built and vendored; if headers seem to be missing something, say so, don't hand-edit.
- **Don't add dependencies without asking** — dependency surface is deliberately minimal.
- **If unsure of exact IEC 61850 semantics** (FC codes, data attribute types, BRCB trigger options), say so and cite the spec section or the relevant `third_party/include` header — don't guess.

- **One narrow, deliberate exception** - Genuinely degraded (vs. loading a real .scd/.icd file), for whenever the live-discovery fallback is used:
GOOSE addressing precision. SCL almost always carries an explicit <GSE><Address> (VLAN/AppID/dst-MAC), so file-based subscriptions filter tightly at the socket level. Discovery only gets this if the device happens to expose the optional DstAddress GoCB attribute over MMS — plenty of real devices don't. When it's missing, reception still works (matches on gocbRef embedded in the frame instead), but the NIC sees more traffic before software-level filtering kicks in, and may need broader (near-promiscuous) reception to catch destination MACs it hasn't explicitly joined. Functionally correct, less efficient. Speed and fragility per connect. SCL = one file transfer + local parse. Discovery = many sequential MMS round-trips (LD list → per-LD LN list → per-LN class/DO/DA enumeration → per-dataset member query), scaling with model size — slower, and more exposed to a device flaking mid-walk than one atomic file transfer. This is exactly the reasoning the existing "no over-the-wire tree discovery" Hard Rule cites — it's why this stays a narrow, explicit fallback rather than replacing the SCL path for devices where SCL-over-file already works fine.

## IPC / Reporting Out
- Normalize C structs (GOOSE frame, MMS report) to JSON.
- Dispatch via a websocket (`ipc_dispatcher/`, loopback-only, `libwebsockets` + `cJSON`) to the
  high-level API (FastAPI/Go) and frontend — this is the "reporting" surface; treat message
  shape as a stable contract, flag breaking changes explicitly. See `ipc_dispatcher/`'s own
  Architecture bullet above for the full envelope shape, threading design, and quality-pairing
  algorithm.
- **Target consumer needs four fields per data point: value, reference, quality, timestamp**
  (timestamp is the lowest priority of the four, but still wanted — confirmed with the actual
  end user this app is built for). `ipc_dispatcher` now assembles all four into one JSON data
  point per value (pairing `q` in as part of this) — the notes below describe what
  `MmsReportRecord`/`GooseSubscriberRecord` give it to work with, which is still useful context
  for anyone touching `mms_report_client`/`goose_subscriber`'s record shape.
- **Field availability today** (`MmsReportEntry` in
  `src/features/mms_report_client/domain/mms_report_client_types.h`, `GooseSubscriberEntry` in
  `src/features/goose_subscriber/domain/goose_subscriber_types.h`) — reference labeling is
  **solved**, quality pairing into one combined data point is **solved** (in `ipc_dispatcher`,
  not in these two structs themselves — they still carry `q` as its own sibling entry, matching
  the wire's own DA-per-entry shape), quality's very *presence* remains **conditional on SCL
  authoring**:
  - **value** — present on both: `entries[i].value` (`MmsValue*`).
  - **reference** — now populated on both transports via `IedModel_getDataSetMemberReferences`
    (`src/features/ied_model/service/ied_model_api.h`), which replays the dataset's own
    already-parsed FCDA order back out as an ordered list of member-reference strings — purely
    local, never over-the-wire (see Hard Rules). MMS report: `entries[i].reference` prefers the
    server's own `ClientReport_getDataReference` when the RCB's `OptFlds` has `DataRef` enabled,
    falling back to the SCL-derived reference (cached once per RCB at `MmsReportClient_start`,
    in `MmsReportClientHandle.memberRefCache`) when the server omits it — `brcbMain` has no
    `DataRef`, so it exercises the fallback path in practice. GOOSE: **always** uses the
    SCL-derived reference (cached once per target in `GooseSubscriberTargetEntry.memberReferences`
    at `GooseSubscription_start`) — GOOSE never carries a reference on the wire, no server-truth
    branch exists. Only `NULL` if the target's `datasetReference` doesn't resolve or the entry
    index is out of range of the dataset's member count.
  - **quality** — still **not a field on either struct**, and that's inherent: in IEC 61850,
    quality (`q`) is a sibling Data Attribute of `stVal`, not embedded in the value itself.
    `Quality` is `typedef uint16_t Quality` (`third_party/include/iec61850_common.h:326`),
    wire-encoded as a 4-byte bitstring `MmsValue`, decoded via
    `Quality_getValidity`/`Quality_isFlagSet` (same header). Whether quality shows up at all
    depends on whether the SCL dataset includes a `q` entry alongside `stVal` for that point.
    `ds1` (see `integration_tests/ied_simulator/src/sim_server.c`) now does — proven end-to-end
    in both `integration_tests/mms_report_client/` and `integration_tests/goose_subscriber/` —
    but this is a per-dataset SCL authoring fact, not a code guarantee: any other IED/dataset
    that doesn't put `q` in its FCDA list simply won't carry a quality entry, and nothing in
    `mms_report_client`/`goose_subscriber` fabricates one. When present, `q` arrives as its own
    `entries[i]`, reference-labeled exactly like any other member (e.g. `...Ind1$q` next to
    `...Ind1$stVal`) at the `mms_report_client`/`goose_subscriber` layer — `ipc_dispatcher` is
    what pairs it with its sibling value (matching by common DO/LN prefix, i.e. everything up to
    the last `$`) into one combined JSON data point; see its Architecture bullet above.
  - **timestamp** — both structs still give one shared, record-level timestamp
    (`record->timestampMs`), not a per-entry timestamp. MMS report: only if `hasTimestamp`
    (driven by the RCB's `OptFlds.TimeStamp`). GOOSE: the frame's own publish time, always
    present. Per-DA `t` attributes, if ever needed per-value, would again show up as their own
    dataset entry, not a struct field. Untouched by the reference-labeling work above.
- **Change-stream rework, at explicit user request**: the websocket output is now a pure stream
  of *changes*, with enough context for the frontend to show what changed and from what:
  1. **The genuine first-ever GI/bootstrap snapshot never reaches the websocket.** The one-shot
     startup GI report (and GOOSE's own equivalent, the very first frame ever for a target) is
     cache-seed-only, never forwarded — see the value-diff filter bullet above for the exact
     mechanism (`shouldForwardAndUpdateCache` treating a never-cached slot as bootstrap,
     unconditionally, regardless of reason). A point that never changes after that seed
     accordingly never reaches the frontend at all — an explicit, accepted tradeoff, not an
     oversight. **This is now ONLY true for that genuine first-ever observation** — the value-diff
     cache is populated once and preserved for the client/subscriber's whole lifetime (never reset
     on reconnect or on a STALE/INVALID_STATE→VALID recovery, see the "Fifth change in the same
     family" bullet under `mms_report_client/`'s own Architecture entry above), so a reconnect's
     or recovery's own fresh GI/redelivered snapshot diffs against the real, preserved last-known
     value instead: a genuine change made while disconnected/stale correctly reaches the websocket
     with a real `previousValue`, exactly like any other change — it is only ever suppressed if the
     value genuinely didn't change.
  2. **`dataPoints` only ever contains points that actually changed** — this falls out
     automatically from (1): `buildEntries` already only emits forwarded entries, and
     `ipc_dispatcher` only ever serializes what it's handed. One deliberate judgment call: the
     existing group-extension pass (a changed quality drags its unchanged value sibling along,
     and vice versa, so quality pairing is never orphaned) is kept as-is — "changed" is judged at
     the (value, quality) *pair* level, not the raw scalar level, so a dragged-along sibling's own
     `previousValue` legitimately equals its current `value`.
  3. **Every forwarded point carries `previousValue`/`previousQuality`** — an owned clone of
     whatever was cached for that exact wire position immediately before this report overwrote it
     (`MmsReportEntry.previousValue`/`GooseSubscriberEntry.previousValue`, captured in
     `shouldForwardAndUpdateCache` regardless of that call's own forward/drop outcome, since a
     later-dragged-in candidate needs its own previous value too). Because the very first-ever
     GI/bootstrap observation always seeds the cache, and the cache is never reset again after
     that (see point 1 above), `previousValue` is populated for essentially every forwarded point
     once a device has been reporting for any length of time, reconnects included — `NULL`/absent
     only in the narrow, pre-existing structural case where a wire position has no cache slot at
     all (`slot < 0` — no `memberRefCache`, or the position isn't covered by it, e.g. parts of the
     Dyn-RCB fallback path). `ipc_dispatcher` converts `previousValue`/`previousQuality` through the exact same
     value/quality codec as the current value, and the JSON writer emits `previousValue`/
     `previousQuality` always present, `null` when absent, matching `quality`'s own existing
     convention.

## Interaction Style
- No fluff, no filler. Peer-to-peer technical register.
- Use opaque pointers / forward declaration to enforce API boundaries.

## Output Format
- Small fixes: just the diff/code, no ceremony.
- New features or architectural changes: (1) where it fits in feature-first layout, (2) the code, (3) Watch Out — link order, memory safety, thread risk.
