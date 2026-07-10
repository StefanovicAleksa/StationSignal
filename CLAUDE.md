# IEC 61850 GOOSE/MMS Reporter — ied_reporter_daemon

## Purpose
Backend daemon that reports IEC 61850 traffic: sniffs GOOSE messages off the wire and
subscribes to MMS report control blocks (BRCB), normalizes both into JSON, and forwards
them to the consuming API layer. This file governs this repo (root = the daemon itself).

## Commands
- Build daemon: **TODO — no CMakeLists.txt or root Makefile exists yet.** `src/main.c` can be
  built manually (same throwaway-linkage-probe convention as the smoke tests below) by
  compiling it together with `src/main_discovery_prompt.c`, every `.c` file under
  `src/orchestration/` and `src/scan_orchestration/`, and all eight `src/features/<feature>/`
  directories (`service`/`data`/`domain`/`utils`), e.g.:
  `gcc -g -Wall -Isrc -idirafter third_party/include src/main.c src/main_discovery_prompt.c
  src/orchestration/*/*.c src/scan_orchestration/*/*.c src/features/*/*/*.c -o
  /tmp/ied_reporter_daemon -Lthird_party/lib -liec61850 -lhal -lmxml -lwebsockets -lcjson
  -lpthread && sudo /tmp/ied_reporter_daemon [host] [mmsPort] [iedName] [interface] [ipcPort]
  [acseAuthPassword] [sclFilePath]` — this is a manual stopgap, not a substitute for a real
  build system; don't invent or guess a permanent build command, ask before assuming one. Only
  `mmsPort`/`interface`/`ipcPort`/`acseAuthPassword` have fallback defaults
  (102/`eth0`/8765/unauthenticated) if omitted:
  - `host` omitted/empty: no hardcoded fallback — runs the continuous background scan
    (`scan_orchestration`) on `interface` instead, streaming discovered IEC 61850 MMS devices
    over its own shared websocket (default port 8766) while an interactive terminal prompt lets
    you pick a device from the live, growing list (or type in an extra candidate IP to verify
    and add) at any time; picking one stops the scan before continuing. See
    `scan_orchestration/`'s own Architecture bullet below.
  - `iedName` omitted/empty: passed through as auto-detect to `Orchestration_run`/
    `_runFromLocalFile` — works only if the SCL declares exactly one `<IED>` (see
    `orchestration/`'s own bullet).
  - `sclFilePath` (optional, 7th slot): if given, skips `scl_bootstrap` entirely and loads
    this local SCL file instead (`Orchestration_runFromLocalFile`) — for devices whose MMS
    server doesn't implement file services (confirmed in practice against OMICRON IED Scout's
    "Simulate IED" mode: a real, connectable MMS/GOOSE device that associates fine but returns
    `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` when browsed — no `.icd`/`.cid`/etc. served at
    all). `host`/`mmsPort` still drive the real live `mms_report_client`/`goose_subscriber`
    connections; `ied_discovery`'s scan/manual-add still works the same beforehand to find
    that host. See `orchestration/`'s own bullet for the stage-by-stage behavior.
  - If `sclFilePath` is **not** given and `Orchestration_run`'s bootstrap stage comes back with
    exactly `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` (the same device class the bullet above
    describes, just without an operator having a local SCL copy in hand), `main.c` automatically
    retries once via `Orchestration_runFromOnlineDiscovery` — builds the model directly from the
    live device's own MMS data model instead of any SCL file at all. See `ied_model_online_loader/`'s
    own Architecture bullet below and the "No over-the-wire tree discovery" Hard Rule's documented
    exception.
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
- Raw-socket loopback smoke test (build manually, no Makefile — throwaway linkage/behavior
  probe): `gcc -g -Wall -Isrc -idirafter third_party/include tools/smoke_tests/goose_loopback_smoke_test.c
  -o /tmp/goose_loopback_smoke_test -Lthird_party/lib -liec61850 -lhal -lpthread && sudo
  /tmp/goose_loopback_smoke_test` — proves a bare `GoosePublisher`/`GooseReceiver` pair
  round-trips a real GOOSE frame over `lo` before trusting that assumption in the E2E test above.
- Run via `sudo` once built — raw socket access required for GOOSE sniffing.

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
  with a **hybrid** rule (`domain/mms_report_client_usecases.c`'s `buildEntries`, via
  `shouldForwardAndUpdateCache`), not reason-for-inclusion alone: an entry whose
  `ClientReport_getReasonForInclusion` bitmask carries a real-change bit
  (`DATA_CHANGE`/`QUALITY_CHANGE`/`DATA_UPDATE`, checked by `MmsReportClientUseCases_hasRealChangeReason`)
  is always forwarded, trusting the server unconditionally. An entry with no such bit
  (`INTEGRITY`/`GI`-only, or reason omitted entirely by servers that never populate
  reason-for-inclusion — confirmed in practice against a real Siemens SIPROTEC device, whose
  RCBs re-send their full dataset every few seconds with no usable reason code) is instead
  gated by a per-position value-diff cache (`MmsReportClientMemberRefCacheEntry.lastForwardedValues`,
  built once per RCB alongside the Gap 4 cache): forwarded only if its value differs from the
  last one actually forwarded for that exact wire position, or if nothing has been forwarded yet.
  That "nothing forwarded yet" case is what lets the one-shot startup `GI` snapshot through
  once — the only chance some stable, never-changing points have to ever deliver their value to
  the frontend at all — while still suppressing every subsequent unflagged periodic resend of an
  unchanged value. A reason-only filter (dropping every `INTEGRITY`/`GI`-only entry outright,
  tried and reverted during development) fails both directions: it starves stable points of any
  initial value, and it does nothing for devices that omit reason-for-inclusion, since an
  unflagged periodic resend then has no basis to be dropped at all. If every entry in a report is
  filtered, `mms_report_client_report_adapter.c`'s `onReport` frees the record without ever
  invoking the caller's report callback (no empty/pointless push downstream).
  **The per-position hybrid filter above, by itself, broke `ipc_dispatcher`'s quality pairing**
  (found against real production traffic): quality (`q`) almost never changes value
  report-to-report and rarely carries a real-change reason on a report triggered by its sibling
  value changing, so after the first `GI` snapshot `q`'s own diff-check drops it on every
  subsequent report while its value sibling (e.g. `stVal`) keeps forwarding — and since
  `ipc_dispatcher`'s `IpcDispatcherUseCases_pairQuality` only pairs entries present in that same
  record, quality showed as `null` forever after the first report. Symmetrically, a genuine
  quality-only change (no value change) left a lone `q` with no forwarded value sibling, which
  `pairQuality` also drops outright — a real quality-degradation event vanished silently too.
  Fixed by making `buildEntries` group-aware: it now runs in three phases (candidate
  collection → per-candidate hybrid-filter decision → a group-extension pass) instead of a
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
- `libcjson.a`/`cJSON.h` (cJSON, MIT) is vendored for `ipc_dispatcher`'s JSON serialization — no JSON library exists elsewhere in this repo; do not hand-roll one. Source built from a sibling checkout at `/home/aleksa/code/ied_reporter/cJSON` (not committed here, same convention) via CMake with `-DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF` (cJSON's own `BUILD_SHARED_AND_STATIC_LIBS` option does **not** suppress the shared build by itself — `BUILD_SHARED_LIBS=OFF` is the flag that actually produces a static-only `libcjson.a`). Push-only in this repo (serialize only, no parse counterpart in production code — cJSON's own parser is only used by tests, to assert JSON shape without brittle string matching).
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
  `src/orchestration/` and `src/scan_orchestration/` are both separate, top-level siblings of
  `src/features/` (not themselves in this feature list) that each sequence a pipeline together —
  see their own bullets below. `ipc_dispatcher`'s lifecycle is owned entirely by orchestration
  (not by `src/main.c` directly) — `main.c` only ever configures it via
  `OrchestrationConfig.ipcDispatcherConfig`, same as every other feature's config; `scan_dispatcher`'s
  lifecycle is likewise owned entirely by `scan_orchestration` (reference-counted by active-scan
  count, not by `src/main.c` directly either) — see `scan_orchestration/`'s own bullet below.
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
- `ied_model/` (implemented) — loads an IED's data model from SCL (`.icd`/`.cid`/`.scd`), gated by an `AccessMode` (REPORT_ONLY/READ_ONLY/READ_AND_WRITE). Public boundary: `src/features/ied_model/service/ied_model_api.h`. `goose_subscriber`/`mms_report_client` should get their subscription targets from here, not by re-parsing SCL themselves. `IedModel_getReportSubscriptionTargets` returns `ReportControlBlockTarget*` (object reference with the correct `.RP.`/`.BR.` segment, buffered flag, dataset reference) rather than a bare string, specifically for `mms_report_client`'s use; `GooseSubscriptionTarget` carries the equivalent `datasetReference` for `goose_subscriber`. `IedModel_getDataSetMemberReferences(handle, datasetReference)` returns the ordered, heap-allocated member-reference strings backing one dataset (index i matches the i-th report/GOOSE entry) — purely local, walks the already-parsed SCL `DataSet`, never over-the-wire (see Hard Rules) — this is what both consumers use to label entries by position. Also exposes `IedModel_listIedNames(path, outError)` — lists every `<IED name="...">` at an SCL file's top level without building a full model, for `orchestration`'s optional IED-name auto-detection (see that feature's own bullet below); a file with zero `<IED>` elements is a valid, non-error, empty result. **Hardened against real-world SCL variation** (found integrating against a real Siemens SIPROTEC device and its exported station SCD): `VLAN-ID`/`APPID` are parsed as hex, not decimal-defaulting `strtoul` base-0 (real values like `"000A"` were silently corrupted to `0` under octal autodetection); `<GSE>`'s `MinTime`/`MaxTime` are now read instead of always defaulting; `<SDI>`-wrapped (structured/array) `<DOI>`/`<DAI>` overrides are now recursed into instead of silently dropped; enumerated `<DAI>` `Val` labels are resolved against the DA's real `<EnumType>` ordinal instead of `atoi`'d (a non-numeric label like `"status-only"` used to silently become ordinal `0`, itself a valid-looking wrong value, not a skip); `LDevice/@ldName` (SCL functional naming) is read and threaded into FCDA/LDevice resolution as a third fallback convention. A vendor pattern where control blocks are embedded as escaped text inside `<Private type="...ControlBlockStorage...">` (seen in a raw, unconfigured Siemens device-type template) is detected and warned about rather than silently producing an empty model — actually parsing that escaped payload is out of scope (vendor-specific, speculative). Deliberately **not** hardened, considered and deferred pending real evidence: duplicate `LDevice/@inst` across multiple `<AccessPoint>`s, `DAI/@ix` array indices, `<Val sGroup="N">` setting-group overrides, dotted `doName`/`daName` FCDA shorthand, non-dash-separated MAC address formats. `GSEControl`'s `datSet` was reconsidered for symmetry with `ReportControl`'s now-optional one and deliberately kept required — every real `GSEControl` sample encountered populates it.
- `mms_report_client/` (implemented) — connects to one IED over MMS, discovers its Report Control Blocks via `ied_model` (never re-parses SCL, never discovers RCBs over the wire), enables reporting on each (`RptEna`[+`GI`], **plus `DatSet`** using `ReportControlBlockTarget.datasetReference` — relying on a server-side default dataset configured only at RCB-creation time turned out to be fragile/version-dependent in practice, so the client always (re-)asserts it explicitly on every enable, matching libiec61850's own reference client example; `TrgOps`/`BufTm`/`IntgPd`/`ConfRev` are still left untouched, exactly as the IED's SCL config has them), and delivers normalized `MmsReportRecord`s via a caller-registered callback (JSON stringification is deferred to `ipc_dispatcher` — no JSON library is vendored). `MmsReportEntry.reference` prefers the server's own `ClientReport_getDataReference` (only present if the RCB's `OptFlds` has `DataRef` set) and falls back to a per-RCB cache of `IedModel_getDataSetMemberReferences` results (built once at `MmsReportClient_start`, never rebuilt on reconnect) when the server omits it. Works under every `ied_model` `AccessMode`, including `REPORT_ONLY`. Public boundary: `src/features/mms_report_client/service/mms_report_client_api.h`. Reconnects with exponential backoff via a dedicated supervisor thread (`hal_thread.h`'s `Thread`/`Semaphore`) driven by `IedConnection`'s state-changed handler — see that header's own doc comments for why the handler can't drive reconnection directly (deadlock risk). MMS host/port are caller-supplied (SCL parsing of the MMS `<ConnectedAP>` IP address is out of scope for now — only GOOSE addressing is parsed by `ied_model`). **Supports ACSE password authentication** via `MmsReportClientConfig.acseAuthPassword` (`data/mms_report_client_auth.c`'s `MmsReportClientAuth_configurePasswordAuth`, same third-party calls as `scl_bootstrap`'s own `data/scl_bootstrap_auth.c` — duplicated rather than shared, since features never reach into each other's `data/`/`domain/` layers, only `service/*_api.h`). `NULL` (default) means every association is unauthenticated, unchanged from before this was added. Unlike `scl_bootstrap` (which tries unauthenticated first, then retries once with a password only on rejection, since it's scanning candidates blind), `mms_report_client` applies the configured password unconditionally from the very first connect attempt — it always targets one already-known IED, so there's no ambiguity to resolve with a retry. Applied once, at `MmsReportClientConnection_create` time, to the one `IedConnection` object that's reused across every reconnect (unlike `scl_bootstrap`'s fresh-connection-per-attempt design), so it covers every future reconnect automatically. Proven end-to-end against a real `ied_simulator` IED in `integration_tests/mms_report_client/`, including both a correct-password-connects and a wrong-password-never-connects case against a real `SimServer_requireAuthentication`-protected instance.
  **Dynamically creates a dataset for RCBs whose SCL declares no `datSet` at all** (`datSet="Dyn"` in SCL `<ReportSettings>` terms — confirmed against a real device, `E13_6MD`/`IEC 61850v2 JA4 station.scd`: every one of its ~174 `ReportControl` elements omits `datSet`, and RCBs there are parented under the specific LN they report on, not just `LLN0`, contradicting the earlier assumption that an RCB's parent LN is always `LLN0`). Previously this feature deliberately never created datasets itself (`setRCBValues` just failed with `IED_ERROR_OBJECT_VALUE_INVALID`, logged, RCB skipped) — that stance blocked reporting entirely on this whole class of device. Now, `data/mms_report_client_connection.c`'s `getOrCreateDynamicDataset` (called from `enableOneTarget` only when `target->datasetReference` is NULL) synthesizes an association-scoped dataset (`IedConnection_createDataSet` with an `@`-prefixed name — destroyed automatically when the connection closes, so no explicit cleanup/leak risk across reconnects) covering **every FC=ST/MX leaf attribute under the RCB's own LN** — "all the variables" for that LN, by this codebase's existing FC=ST/MX "reportable" convention (see `IedModel_getReadTargets`). The member list comes from a new `ied_model` accessor, `IedModel_getReportableAttributeReferencesForLogicalNode(handle, lnReference)` (`ReportControlBlockTarget` gained an `lnReference` field for this), purely local like every other `ied_model` accessor — never over-the-wire. `mms_report_client_api.c`'s `buildMemberRefCache` uses this same accessor (not just the connection layer) to seed the RCB's reference-labeling/value-diff cache up front, so dynamic RCBs get the exact same reference-labeling/hybrid-event-filter treatment as SCL-declared ones, no special-casing downstream. A new domain usecase, `MmsReportClientUseCases_buildWireMemberReferences`, converts this codebase's standard `"$"`-joined reference form to `IedConnection_createDataSet`'s required dot/bracket wire form. A per-connect-cycle cache (LN reference → generated dataset name, built fresh in `enableAllTargets`, discarded at the end) de-dupes dataset creation across an LN's redundant reserved RCB instances (e.g. `urcbA..urcbJ` all sharing one LN) — without it, a device like `E13_6MD` would attempt to create the same dataset ~10× over just for one LN's reserved slots. **Known, deliberately unsolved limitations**: no chunking against a device's `maxAttributes` cap (an LN with more reportable leaves than the cap fails `createDataSet` for that LN, falls back to the pre-existing failure mode); no handling of a device's total dataset-count cap being smaller than its unique-LN count (per-LN scope, not per-LDevice) — both are honest, unresolved trade-offs from a design discussion that intentionally deferred multiple stakeholder-specific scope questions rather than guessing. Proven end-to-end against a real `ied_simulator` IED in `integration_tests/mms_report_client/` (a fixture RCB parented under a non-`LLN0` LN, no `datSet` at all, mirroring `E13_6MD`'s real shape).
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
  `integration_tests/goose_subscriber/` (requires `sudo` — see Commands).
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
  per IEC 61850-7-3) — this function has no way to know which specific CODEDENUM a given
  bitstring represents (Dbpos and Tcmd share the same wire type but different meanings), and
  guessing a decoded label without per-type verification would violate this repo's own "don't
  guess IEC 61850 semantics" rule; the raw bit pattern is always correct regardless. Quality's
  own bitstring never reaches this path at all — `IpcDispatcherUseCases_pairQuality` excludes
  every `q`-named entry from ever being treated as a value, routing it to `_decodeQuality`
  instead (see above), so this addition can't double-decode or conflict with quality handling.
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
  hasTimestamp, timestampMs?, dataPoints: [{reference, value, quality}]}`.
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
  config structs across the public boundary. The interactive terminal loop (scan, print a
  numbered list, let the operator type a number to pick or an IP to add-and-verify a manual
  candidate) is deliberately kept **out** of this feature entirely, in a separate thin adapter,
  `src/main_discovery_prompt.c` — the user who requested this explicitly said the interaction
  medium will likely be replaced later (e.g. driven by the API layer this daemon reports to), so
  it must be swappable without touching `ied_discovery` itself. Proven end-to-end (`verifyHost`
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
  one scan's currently-announced hosts, for an in-process caller like
  `main_discovery_prompt.c` that needs the live, growing list without connecting to the
  daemon's own websocket as a client of itself).
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

## Interaction Style
- No fluff, no filler. Peer-to-peer technical register.
- Use opaque pointers / forward declaration to enforce API boundaries.

## Output Format
- Small fixes: just the diff/code, no ceremony.
- New features or architectural changes: (1) where it fits in feature-first layout, (2) the code, (3) Watch Out — link order, memory safety, thread risk.
