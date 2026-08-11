# IEC 61850 GOOSE/MMS Reporter — station_signal_daemon

## Purpose
Backend daemon that reports IEC 61850 traffic: sniffs GOOSE messages off the wire and
subscribes to MMS report control blocks (BRCB), normalizes both into JSON, and forwards
them to the consuming API layer. This file governs this repo (root = the daemon itself).
Full incident-by-incident history behind every decision below lives in `CHANGELOG.md` — this
file only describes current-state facts.

## Commands
- Build daemon: **TODO — no CMakeLists.txt or root Makefile exists yet.** `src/main.c` can be
  built manually (same throwaway-linkage-probe convention as the smoke tests below) by
  compiling it together with every `.c` file under `src/orchestration/`,
  `src/scan_orchestration/`, and `src/device_manager/`, and all ten `src/features/<feature>/`
  directories (`service`/`data`/`domain`/`utils`), e.g.:
  `gcc -g -Wall -Isrc -idirafter third_party/include src/main.c
  src/orchestration/*/*.c src/scan_orchestration/*/*.c src/device_manager/*/*.c
  src/features/*/*/*.c -o /tmp/station_signal_daemon -Lthird_party/lib -liec61850 -lhal -lmxml
  -lwebsockets -lcjson -lpthread && /tmp/station_signal_daemon` (also wrapped by
  `rebuild_proj.sh`) — a manual stopgap, not a substitute for a real build system; don't invent
  or guess a permanent build command, ask before assuming one. `sudo` is only required once a
  client asks the daemon to actually report on a device over GOOSE (raw socket) — the process
  itself starts and idles fine without it.
  - `./setup_project.sh` regenerates all five vendored archives —
    `third_party/lib/{libiec61850.a,libhal.a,libcjson.a,libmxml.a,libwebsockets.a}` — and
    `third_party/include/` from the vendored source in the `third_party_src/{libiec61850,cJSON,
    mxml,libwebsockets}` git submodules (`git submodule update --init --recursive` if not yet
    checked out — governed by the **root** `station_signal` repo's `.gitmodules`, since the
    gitlinks live under this directory but are recorded in the top-level tree). libiec61850 and
    libwebsockets build via CMake; cJSON via CMake with `-DBUILD_SHARED_LIBS=OFF`; mxml via
    autotools (`./configure --disable-shared`, no CMakeLists.txt at the pinned v3.3.1 commit).
    libwebsockets is built with `-DLWS_WITH_SSL=OFF` to match the currently-vendored archive
    (confirmed via its `lws_config.h` and `nm` symbol inspection — no OpenSSL symbols, only the
    `*_no_ssl` stubs). **Not run automatically and not part of the normal build** —
    `third_party/{include,lib}` as currently committed is the source of truth (see the "Don't
    touch `third_party/`" Hard Rule). Only run it if you genuinely need to rebuild from source
    (different build flags, a version bump, targeting a new architecture such as a Raspberry
    Pi's ARM — this script builds natively, no cross-compilation, so run it on the target
    machine — or debugging by stepping into library source), and review the resulting
    `git diff third_party/` before committing it.
  - `main.c` takes **no arguments** and has no terminal/CLI surface — it's a pure background
    process-runner. It creates `device_manager` + `scan_orchestration` + `control_dispatcher`,
    starts the one always-on control websocket (default `127.0.0.1:8767`), and blocks until
    `SIGINT`/`SIGTERM`. Every device/scan lifecycle action goes exclusively through that control
    websocket's four JSON commands below. See `control_dispatcher/`'s own Architecture bullet for
    the full envelope shape.
  - `START_REPORTING {host, mmsPort, iedName?, interfaceId, sclFilePath?, acseAuthPassword?,
    accessMode?}` → `{deviceId, wsPort}`: starts one IED's full MMS+GOOSE pipeline via
    `DeviceManager_startReporting`, auto-assigning its own `ipc_dispatcher` port from
    `device_manager`'s range (default 9000-9999). `iedName` omitted/empty auto-detects (needs
    exactly one `<IED>` in SCL) unless `sclFilePath` is also given, in which case `iedName` is
    required. `sclFilePath` omitted (common case): loads structure via `scl_bootstrap`, retrying
    once via `Orchestration_runFromOnlineDiscovery` if it returns exactly
    `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` (a connectable device with no SCL file service —
    see `ied_model_online_loader/`'s bullet and the "No over-the-wire tree discovery" Hard Rule).
    `sclFilePath` given: skips `scl_bootstrap`, loads that local file (`Orchestration_runFromLocalFile`).
  - `STOP_REPORTING {deviceId}` → `{deviceId}`: tears down that device, freeing its port. Accepts
    `{host, mmsPort}` instead of `deviceId` as an alternate form — recovery path for a caller that
    never obtained or has lost track of a device's id (see `device_manager`'s own bullet,
    `DeviceManager_stopReportingByAddress`); resolves to whichever deviceId currently occupies
    that address, then behaves identically to the `deviceId` form (including
    `DEVICE_MANAGER_ERR_START_IN_PROGRESS` if it's still mid-start), and echoes the *resolved*
    deviceId back in the response either way. `DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND` if nothing is
    registered at that address — a legitimate "already clean" outcome for this use case, not
    necessarily a real failure.
  - `START_SCAN {interfaceId, mmsPort, sweepIntervalMs?}` → `{scanId}`: starts a continuous
    background subnet scan (`scan_orchestration`), streaming discovered devices over the shared
    `scan_dispatcher` websocket (default port 8766, one shared instance per concurrent scan).
    Starts that websocket on the first concurrently-active scan (0→1).
  - `STOP_SCAN {scanId}` → `{scanId}`: stops that scan; tears down `scan_dispatcher` at 1→0.
- Build + run IED simulator (integration test fixture): `cd integration_tests/ied_simulator && make`
- Generate simulator model: `./integration_tests/ied_simulator/scripts/generate_model.sh`
- Run unit tests: `cd tests && make run` — strictly unit, no file I/O beyond two self-contained
  temp-file cases (`ied_model`, `orchestration`), fast. `tests/Makefile` does **not**
  auto-discover — a new feature's unit tests need a Makefile edit, not just new files.
- `cd integration_tests/<feature> && make run` (or `sudo make run` where noted) runs that
  feature's E2E suite against a real `ied_simulator` "Reporter1" IED in-process (or, for the
  transport-only features, a hand-rolled minimal RFC6455 test client, no simulator needed).
  **Needs `sudo`** only for suites touching real GOOSE (raw Ethernet socket): `goose_subscriber`,
  `orchestration`, `device_manager`, and `control_dispatcher`'s third (real-device) case. Every
  other suite is plain TCP/loopback MMS. `orchestration`'s suite also runs
  `test_onlineDiscoveryFallback_afterNoSclFileFound_endToEnd`, proving the online-discovery
  fallback end to end. `ied_discovery`'s suite deliberately does not exercise a real
  `IedDiscovery_scanSubnet`/`getifaddrs` sweep (topology-dependent) — verify manually by running
  the daemon and issuing `START_SCAN` against a machine with a real neighbor IED.
- Raw-socket loopback smoke test (build manually, no Makefile — throwaway linkage/behavior
  probe): `gcc -g -Wall -Isrc -idirafter third_party/include
  tools/smoke_tests/goose_loopback_smoke_test.c -o /tmp/goose_loopback_smoke_test
  -Lthird_party/lib -liec61850 -lhal -lpthread && sudo /tmp/goose_loopback_smoke_test` — proves a
  bare `GoosePublisher`/`GooseReceiver` pair round-trips a real GOOSE frame over `lo`.
- Run via `sudo` once built — raw socket access required for GOOSE sniffing.
- `./run_all_tests.sh` runs every suite above in one pass, re-execing itself under `sudo` if not
  already root, and aggregates every Unity `N Tests M Failures P Ignored` line into one grand
  total. **This suite list is hand-maintained, not auto-discovered** — adding/removing a test
  suite means updating `run_all_tests.sh`'s `run_suite` calls in the same change.

## Current State (update as this evolves)
`main.c` takes zero arguments (`int main(void)`) and only creates `device_manager` +
`scan_orchestration` + `control_dispatcher`, starts the control websocket, and blocks on
`SIGINT`/`SIGTERM` until torn down in reverse order. All ten `src/features/` plus
`src/orchestration/`, `src/scan_orchestration/`, and `src/device_manager/` are implemented (see
Architecture below). There is no boot-time device, no argv, and no interactive discovery
prompt — `control_dispatcher`'s four JSON commands (see Commands above) are the daemon's only
way to start/stop anything. Full history of how `main.c` got here (multi-IED support, the
discovery-prompt removal, the earlier argv layout) is in `CHANGELOG.md`.

Every historical bugfix (rollback ordering, reconnect races, value-diff cache semantics,
quality-pairing, GI removal/reinstatement, dynamic dataset creation, EntryID resumption, Gap-4
decomposition type-checking) is folded into the current-state description of the relevant
feature under Architecture below — see `CHANGELOG.md` for the full root-cause narratives.

## Testing
- `tests/<feature>/test_*.c` — strict unit tests (Unity framework), one file per source file.
  Fast, hermetic: fixtures are tiny in-memory models/mxml nodes built via the dynamic model API,
  or handles constructed directly rather than mocked. `tests/Makefile` is an explicit `TESTS`
  list plus one hand-written build rule per binary — it does **not** auto-discover, so a new
  feature's tests need a Makefile edit. A feature's data-layer/loader code is deliberately *not*
  unit-tested here if its correctness is better proven end-to-end (see `ied_model`) — extend the
  E2E test instead of duplicating coverage. Two self-contained temp-file cases exist
  (`tests/ied_model/`, `tests/orchestration/test_orchestration_staging.c`) — the only file I/O
  permitted here. `tests/ied_discovery/` covers pure CIDR math, real `getifaddrs()` against
  loopback only, and one deterministic `maxHosts`-ceiling assertion — never a real subnet scan.
  Each dispatcher's own `tests/<name>_dispatcher/` covers its duplicated ring-buffer/JSON
  writer/api wiring, including a real loopback bind; `tests/control_dispatcher/` additionally
  covers the request queue and JSON parser (this codebase's first production `cJSON_Parse` call).
  `tests/scan_orchestration/`/`tests/device_manager/` cover seen-set dedup/port allocation and
  their own two-phase-locked registries against fake handles/a nonexistent interface — never a
  real scan/`Orchestration_run*` call (each feature's own `integration_tests/` job).
  `tests/mms_dataset_manager/` covers only that feature's **pure** layer (reference conversion,
  DO-atomic/whole-device chunking, budget arithmetic) — its data layer is deliberately not
  unit-tested, since every path in it needs a live server and is already proven end-to-end
  through `integration_tests/mms_report_client/`, which exercises the whole dataset lifecycle
  through the new boundary (same "prove it E2E rather than duplicate coverage" call as
  `ied_model`'s loader). There is deliberately **no** `integration_tests/mms_dataset_manager/`.
- `integration_tests/<feature>/` — E2E tests of a real feature's public API against a real,
  self-authored fixture file, also Unity-based. Unlike `ied_simulator/` (below), these
  intentionally link against real `src/` code — the decoupling rule is about the IED simulator
  acting as a fake external device, not about integration tests of our own features.

## Architecture — Feature-First
- Repo root is the daemon. `integration_tests/ied_simulator/` holds IED simulators, fully
  decoupled from prod source; other `integration_tests/<feature>/` dirs hold E2E tests of real
  features and do link against `src/`.
- Inside `src/features/<feature_name>/`: `*_types.h` (domain entities/enums/opaque pointers),
  `*_api.h` (public service-layer API), `*.c` (implementation, third-party integration). Features
  with real business logic worth isolating (see `ied_model/`) nest the above inside
  `domain/`/`data/`/`utils/`/`service/` subfolders: `domain/` holds pure logic with no
  third-party includes, `data/` holds third-party/file/library integration, `service/*_api.h` is
  still the one public header other features may include. Simple features skip the subfolders.
- `src/main.c` wires dependencies only — no business logic. Ten `src/features/`: `scl_bootstrap/`,
  `ied_model/`, `goose_subscriber/`, `mms_report_client/`, `ipc_dispatcher/`, `ied_discovery/`,
  `ied_model_online_loader/`, `scan_dispatcher/`, `control_dispatcher/`,
  `mms_dataset_manager/` — all implemented. Don't
  invent unrelated features without being asked; each addition beyond the original five was an
  explicit user request (see `CHANGELOG.md`). `src/orchestration/`, `src/scan_orchestration/`, and
  `src/device_manager/` are three top-level siblings of `src/features/` that each sequence a
  pipeline (not "features" themselves). `ipc_dispatcher`'s lifecycle is owned entirely by
  `orchestration`; `scan_dispatcher`'s by `scan_orchestration` (refcounted by active-scan count);
  `control_dispatcher`'s by `main.c` directly (a single, shared, always-on control channel).
- `scl_bootstrap/` — one-shot, synchronous bootstrap/probe utility: given a caller-supplied list
  of candidate hosts, TCP-probes each for MMS on a given port, and for each found, browses its
  file directory and fetches one SCL file (`.icd`/`.cid`/`.scd`/`.ssd`/`.sed`) over standard MMS
  file services. Does **not** load into `ied_model` and does **not** enable reporting — purely
  discovery. Public boundary: `src/features/scl_bootstrap/service/scl_bootstrap_api.h`. No
  `_start`/`_stop` pair — `SclBootstrap_scanAndFetch` blocks and returns the complete result set
  in one call. Optionally gated behind ACSE password auth (one retry). Also exposes
  `SclBootstrap_tcpProbeOnly` (its own async bounded-concurrency TCP-probe state machine) purely
  so `ied_discovery` can reuse it without pulling in the full feature. Proven end-to-end
  (including auth-retry and negative-path cases) in `integration_tests/scl_bootstrap/`.
- `ied_model/` — loads an IED's data model from SCL (`.icd`/`.cid`/`.scd`), gated by an
  `AccessMode` (REPORT_ONLY/READ_ONLY/READ_AND_WRITE). Public boundary:
  `src/features/ied_model/service/ied_model_api.h`. `goose_subscriber`/`mms_report_client` get
  their subscription targets from here, never by re-parsing SCL themselves.
  `IedModel_getReportSubscriptionTargets`/`GooseSubscriptionTarget` carry the object reference,
  buffered flag, dataset reference, `lnReference`, and (GOOSE) optional VLAN/APPID/dst-MAC.
  `IedModel_getDataSetMemberReferences` returns the ordered member-reference strings backing one
  dataset — purely local, never over-the-wire (see Hard Rules). `IedModel_listIedNames` lists
  every `<IED name="...">` for `orchestration`'s IED-name auto-detection.
  `IedModel_getReportableAttributeReferencesForLogicalNode` returns every FC=ST/MX leaf under one
  LN, used by `mms_report_client`'s dynamic-dataset synthesis. `IedModel_getDynDataSetMax`/
  `_getDynDataSetMaxAttributes` expose SCL's `<Services><DynDataSet max="N" maxAttributes="M"/>`
  (a direct child of `<IED>`, a sibling of `<AccessPoint>` — not nested under `Server`), parsed by
  `ied_model_scl_loader.c` and stored on the handle; `-1` if `<Services>`/`<DynDataSet>` is absent
  or a given attribute is missing, `0` a real distinct "device declares zero capacity" value — the
  first bare-scalar-returning accessors in this feature, used by `mms_report_client`'s dynamic
  dataset budget/chunking (see that feature's own bullet). `IedModel_getDataSetMemberLeafWireTypes`
  + `IedModel_dataAttributeTypeMatchesMmsType` give Gap-4 decomposition's reorder step a per-leaf
  expected-vs-actual type signal (disambiguation only, never a reject gate — see that step's own
  bullet below) alongside the count check. `IedModel_wrapDynamicModel(model, iedName, mode)` wraps an
  already-built dynamic `IedModel*` (from `ied_model_online_loader`) the same way
  `IedModel_loadFromFile` wraps an SCL-parsed one, so every accessor behaves identically. Hardened
  against real-world SCL variation: hex-parsed VLAN-ID/APPID, `<GSE>` MinTime/MaxTime,
  `<SDI>`-wrapped structured/array overrides recursed into, enumerated `<DAI>` `Val` labels
  resolved against the real `<EnumType>` ordinal (not `atoi`'d), `LDevice/@ldName` as a third
  FCDA/LDevice resolution fallback, and **dotted FCDA shorthand** — IEC 61850-6 lets an FCDA's
  `doName`/`daName` carry a nested path (`doName="SeqA.c3"` an SDO chain, `daName="mag.f"` a BDA
  chain), which `IedModelUtils_buildFcdaVariableName` expands to this codebase's `"$"`-joined wire
  form (a `.` is never legal inside a single SCL name, so the rewrite is unconditional and can't
  corrupt a plain one). Real ABB exports use both forms; no Siemens export in this repo does.
  Deliberately **not** hardened (considered, deferred): duplicate `LDevice/@inst`, `DAI/@ix` array
  indices, `<Val sGroup="N">` overrides, non-dash MAC formats. `GSEControl`'s `datSet` is optional
  per the schema (`tControlWithIEDName`, same as `ReportControl`'s) but **required by this daemon
  for a GoCB to be subscribable** — GOOSE carries no reference on the wire, so with no dataset
  there is no member list to decode frame positions against; such a GoCB is skipped with its own
  named log line, deliberately not the "malformed" one (a real ABB file has six).
  **Siemens `<Private type="Siemens-ControlBlockStorage">` RCBs are parsed too**: a Siemens System
  Configurator export can move a device's entire `<ReportControl>` set out of the SCL schema tree
  into per-LN private elements whose text is the same `<ReportControl>` XML, entity-escaped, behind
  a vendor `"<name>|"` prefix. `buildReportControls` unescapes and feeds each one through the exact
  same `processReportControlNode` path a literal element takes — no divergent behavior between the
  two origins. Not hypothetical: one revision of a real 67-IED station SCD emitted **all 6478**
  SIPROTEC 4 (DIGSI 4) RCBs this way while leaving every SIPROTEC 5 IED untouched, so the same
  station file shows zero report control blocks for those devices in OMICRON IED Scout (which
  doesn't read vendor private storage) and a full, byte-identical target set here.
  **`LLN0` is exempt from the LN-category filter entirely** — `IedModelLnCategory_isAlwaysIncludedLnClass`
  (LLN0 only, deliberately not `LPHD` or the rest of the `L` group), carried per-LN on
  `IedModelLnCategoryEntry.alwaysInclude` and honored by
  `IedModel_getReportableAttributeReferencesForWholeDevice` plus, per data point, by
  `mms_report_client`/`goose_subscriber`'s own `leafAlwaysInclude` slot arrays. An LD's own
  `Mod`/`Beh`/`Health` is what tells a consumer that whole Logical Device is alive, so a
  Control-only connect must not silently drop it (which it did, until this exemption — LLN0's
  group letter is `L`, so it classifies `OTHER`). The exemption is **orthogonal to the category**:
  LLN0 still reports `OTHER` through `IedModel_getCategoryForMemberReference`, so
  `ipc_dispatcher`'s outbound `category` field is unchanged. A `NULL`/unresolvable LN never earns
  an exemption (the opposite default from `categoryForLn`'s `OTHER`).
  **LN-category classification is vendor-neutral by construction, not by enumeration.**
  `kGroupLetterTable` covers all sixteen IEC 61850-7-4 Ed2 group letters (`Q` → MEASUREMENT for
  power quality, `F`/`K` → OTHER); the domain-extension groups (`D` DER/7-420, `H` hydro/7-410,
  `W` wind/61400-25) are deliberately not enumerated and land in OTHER along with any letter that
  isn't a group at all (e.g. Siemens' own `USER` LNs). The SCL path reads `lnClass` directly, so
  it's exact. The no-SCL online-discovery path (`IedModelLnCategory_forWireInstanceName`, which
  only gets `prefix+lnClass+inst` concatenated) tries the standard-class dictionary first, then
  falls back to reading the **last four characters** as the class — IEC 61850-6 fixes an LN class
  at exactly four characters, so that classifies classes the dictionary omits and vendor
  extensions alike. Across three real station files the dictionary alone missed 23 classes
  covering 4177 LN instances (`LPDI`, `LPDO`, `USER`, `QVVR`, `PTUF`, `CBAY`, …); the fallback
  covers them without an entry each.
- `mms_report_client/` — connects to one IED over MMS, discovers its Report Control Blocks via
  `ied_model` (never re-parses SCL, never discovers RCBs over the wire), enables reporting on
  each, and delivers normalized `MmsReportRecord`s via a caller-registered callback (JSON
  stringification deferred to `ipc_dispatcher`). Public boundary:
  `src/features/mms_report_client/service/mms_report_client_api.h`. Works under every
  `AccessMode`, including `REPORT_ONLY`. Reconnects with exponential backoff via a dedicated
  supervisor thread driven by `IedConnection`'s state-changed handler.
  **Enable behavior — a sequential, per-element, read-back-verified sequence.** An enable is
  **never** one bundled `setRCBValues`; it is a series of SINGLE-element writes in strict
  IEC 61850-7-2 order, because that spec makes `DatSet`/`OptFlds`/`TrgOps`/`BufTm`/`IntgPd`
  writable only while `RptEna` is FALSE, and because a bundled rejection names the *request*,
  never the *element* (see `CHANGELOG.md` for the four reverted commits that failed to diagnose
  a real SIPROTEC without this). The dataset is resolved **first**, before any local mutation or
  wire write — a target that resolves no dataset is left completely untouched, with no writes and
  no report handler installed. Then, per target: **0.** `RptEna=false`, only if the device reports
  it already enabled (so the config writes below are legal — the common buffered-reconnect case);
  **1.** `DatSet`, always explicitly re-asserted, never a server-side default — *fatal* if
  rejected **unless that step's own read-back already shows exactly the dataset that was
  resolved**, in which case the binding is correct regardless and the enable continues (SCL's
  `<Services><ReportSettings datSet="Conf">` declares DatSet configurable offline only, not
  online-writable, and a device enforcing it refuses every such write — declared that way on
  every ABB IED in a real 48-IED station file, against Siemens' `datSet="Dyn"` in both of its
  own; what gets written is that RCB's own SCL-declared `datSet`, i.e. the binding the device
  already has, so refusing it says nothing about whether the RCB can report, and aborting would
  take down all 231 of that file's RCBs). The read-back costs nothing extra — `runRcbStep`
  already performs it unconditionally. A read-back that itself fails leaves this false, so a
  refused write with no confirmation is still fatal; **2.** `OptFlds` with `RPT_OPT_ENTRY_ID` OR'd in, buffered RCBs only, only if the bit
  isn't already set; **3.** `TrgOps` (next paragraph); **4.** `EntryID`, buffered RCBs with a
  cached resume point; **5.** `RptEna=true`, the step that actually activates the RCB — *fatal*
  if rejected; **6.** `GI` (with one exception, two paragraphs down) purely to force a
  deterministic snapshot — never trusted/forwarded on its own merits, it lands on the same
  bootstrap-suppression path any first observation would. Only steps 1 and 5 abort the enable;
  a failure in 0/2/3/4/6 is logged loudly and the sequence continues, since a degraded-but-enabled
  RCB beats no RCB. `BufTm`/`IntgPd`/`ConfRev` are never written at any step.
  **Steps 2 and 3 judge against the state left behind by step 1, never against the entry snapshot.**
  A real SIPROTEC clears `TrgOps` *and* `OptFlds` whenever `DatSet` is written to a **different**
  value, so an RCB that arrived already carrying `dchg|qchg|gi` has it wiped by step 1; deciding
  from the pre-step-1 snapshot skipped the rewrite and enabled the RCB with `TrgOps=0x0`, which can
  never emit a change report and does not even answer step 6's GI — three of eighteen enabled RCBs
  were silently dark on real hardware while every write returned `IED_ERROR_OK` (see `CHANGELOG.md`).
  `runRcbStep` therefore hands its read-back back to the caller (`outLive`, no extra round-trip) and
  `enableOneTarget` drives both skip-conditions from that `postDatSetState`. **A failed read-back
  falls through to writing, never to skipping** — rewriting a value the device may already hold is
  harmless; skipping risks leaving the attribute wiped. This is *not* a buffered/unbuffered split
  (the obvious misreading): the causal variable is whether `DatSet` changed, and buffered RCBs are
  equally exposed via `OptFlds`, where the same bug would silently kill EntryID resumption.
  **An RCB that cannot report is counted as FAILED, not enabled**: after the sequence, if the live
  `TrgOps` has neither `dchg` nor `qchg` the RCB is silent by construction, so it logs `FAILED`,
  uninstalls its handler and fires `rcbStatusCallback(false, …)`. Judged on the change triggers
  only — missing just `TRG_OPT_GI` is degraded (changes still flow, only the enable snapshot is
  lost) and stays a warning. This is what makes the cycle summary's "no failures" trustworthy.
  **Every executed step is verified by its own independent read-back** (`runRcbStep` →
  `readAndLogLiveRcbState`), logging `VERIFIED` / `NOT APPLIED` / `unverifiable` — this exists for
  the failure mode that has no error code at all, a device returning `IED_ERROR_OK` for a write it
  then silently never applies. `OptFlds`/`TrgOps` verify by *containment*, never equality (this
  feature only ORs bits in); `EntryID`/`GI` are write-only in effect and reported as unverifiable
  rather than falsely confirmed; an `RptEna=true` that returns OK but reads back false is a loud
  named warning but deliberately **not** a failure. The `TEMPORARILY_UNAVAILABLE` retry loop is
  per-step. **The resulting log volume (and the extra read per step) is a deliberate temporary
  diagnostic posture, not a steady state** — see `CHANGELOG.md` for what to trim first.
  **"Not needed" is a distinct outcome from "failed", and only one of them is a problem.** A Dyn
  RCB left with no cluster to cover (clustering ran out of clusters before it ran out of slots —
  routine on hardware carrying dozens of redundant spare RCB instances) is logged calmly as *not
  needed*, is left completely untouched, and deliberately fires **no** `rcbStatusCallback` at all:
  that callback's contract is "once per enable *attempt*", and no attempt is made. An RCB that
  genuinely needed a dataset and couldn't get one (budget exhausted, `createDataSet` rejected, no
  wire-convertible members, malformed LN reference) is logged loudly as **FAILED** and still fires
  `enabled=false`. `MmsDatasetManagerProvisioning_getOrCreateDataset`'s `outWasNeeded` out-param is the only signal
  separating the two; before it they were byte-identical, which buried real failures among benign
  spares. Each connect cycle ends with one summary line — `N enabled, N not needed, N FAILED` —
  so the question "did anything actually break this cycle?" is answerable without counting per-RCB
  lines by hand.
  **`TrgOps.dchg`/`qchg`/`gi` are proactively OR'd into every RCB's own current TrgOps on every
  enable** (never clearing anything already set) — a real device (via OMICRON IED Scout's
  "Simulate IED" feature) was found with an RCB's live TrgOps carrying ONLY General Interrogation,
  meaning it would never generate a report on an actual data/quality change, only the one-time GI
  snapshot on enable (itself invisible to the frontend via bootstrap-suppression) — every
  subsequent value change on such an RCB was silently invisible end-to-end, with nothing to log on
  either side, since the server genuinely never sends anything to see. GI is included in the OR
  because this feature's own GI request below depends on it being enabled server-side to be honored
  at all, per IEC 61850. Deliberately does **not** touch `TrgOps.dupd`/`integrity` — integrity is a
  periodic/timer-based trigger, and this feature is deliberately, strictly event-driven; enabling it
  would reintroduce exactly the "periodic traffic that looks like an event" problem the value-diff
  cache exists to filter out. Only written back if at least one needed bit is missing, to avoid
  touching this attribute on every single reconnect once a device has accepted it once — same
  minimal-footprint posture as the OptFlds.EntryID OR below. `BufTm`/`IntgPd`/`ConfRev` are still
  left untouched. Written as step 3, i.e. **before** the `RptEna=true` of step 5: most servers,
  including this repo's own `ied_simulator`, reject a TrgOps change once the RCB is enabled, and
  `integration_tests/mms_report_client`'s `test_dynamicDataset_giOnlyRcb_reportsRealChangeAfterTrgOpsFix`
  fails with `IED_ERROR_TEMPORARILY_UNAVAILABLE` if this write is ever deferred past the enable —
  it is the standing guard on the step order.
  **GI is skipped on a buffered RCB's (re)enable whenever it has a valid EntryID to resume from**
  (`MmsReportClientUseCases_shouldRequestGiOnEnable`) — a real device was found enqueuing its own
  GI response as a brand-new entry in that RCB's buffered backlog (fresh, ever-increasing EntryID,
  byte-identical stale content), so every reconnect piled one more near-duplicate snapshot into the
  buffer; replayed through the single-slot value-diff cache, long-settled values looked like they
  were changing again well beyond what actually happened during the outage. A buffered RCB's own
  EntryID resume already guarantees delivery of everything that changed while disconnected, so GI
  adds nothing there. GI is still requested unconditionally for every unbuffered RCB (no buffer at
  all — GI is the only way to catch a change made while disconnected) and for a buffered RCB with
  nothing to resume from yet (first-ever enable, or after an EntryID rejection resets the cache back
  to `NULL`).
  **Event-driven filtering** (mirrors `goose_subscriber` exactly): a per-position value-diff cache
  forwards an entry only if it differs from the last one actually forwarded — **never** trusts the
  server's `ReasonForInclusion` bitmask to bypass this check (independently, combinably set by
  real devices — see `CHANGELOG.md`). Comparison uses `valuesAreSemanticallyEqual`, not raw
  `MmsValue_equals`: `MMS_UTC_TIME`/`MMS_BIT_STRING` ignore the trailing `TimeQuality` byte and
  unused padding bits respectively, both of which can legitimately vary between semantically
  identical real-hardware reports. `cached == NULL` is a bootstrap event: silently seeded, never
  forwarded. **The cache is never reset**, on either this feature or `goose_subscriber` —
  populated once per position, preserved for the client's whole lifetime — so a reconnect's fresh
  `GI` snapshot diffs against the real preserved last-known value: a genuine change made while
  disconnected forwards with a real `previousValue`. **One deliberate, narrow exception**: if a
  target's resolved dataset *identity* itself changes between connects (`resolvedDatasetReference`
  mismatch — a genuine shape change, not the everyday case), the cache is rebuilt from scratch back
  to bootstrap, since old slot indices no longer mean the same wire position under the new shape —
  logged explicitly (`ensureLnFallbackMemberRefCache`/`refreshPulledMemberRefCache`) whenever it
  actually fires, precisely so a dataset-identity churn silently swallowing real changes (see
  `CHANGELOG.md`) is diagnosable from a log capture instead of looking identical to "nothing ever
  changed."
  **Quality pairing**: a candidate that didn't individually qualify still forwards if any other
  candidate resolving to the same "anchor" (nearest structural ancestor, an ancestor walk over
  `$`-segments, not a last-`$` strip) does — value drags quality along and vice versa; a `NULL`
  value is excluded from being dragged in. `IpcDispatcherUseCases_pairQuality` mirrors this.
  **Which dataset each RCB reports on is NOT decided here** — that is entirely
  `mms_dataset_manager/`'s job (see its own bullet below): discovery of what already exists on the
  device, whole-device cluster planning, self-creation, budgets, adoption and cleanup all live
  there. This feature calls `MmsDatasetManager_beginCycle` once per connect, asks
  `MmsDatasetManager_resolveForTarget` per RCB, binds whatever comes back as step 1 below, and
  calls `_endCycle` when the loop finishes (plus `_cleanupOnStop` on the way down, before
  `IedConnection_close`). It keeps only the *decode-shape reconciliation* that follows a
  resolution (`refreshPulledMemberRefCache`/`ensureLnFallbackMemberRefCache`), because that
  mutates this feature's own member-ref/value-diff cache under its own lock. A resolution's
  `wasNeeded` flag is what separates a benign unused spare RCB slot from a real failure.
  **EntryID resumption**: the last observed `ClientReport_getEntryId` per buffered RCB is
  persisted and reused on re-enable so a reconnect resumes instead of a full backlog redelivery.
  **Gap-4 decomposition**: a structured attribute's wire value is flattened, then reordered
  (`reorderFlattenedToMatchReferences`) to match a locally-resolved SCL reference list — "q"/"t"
  are matched by their fixed, CDC-independent wire type; any other leaf (e.g. a DPC's `stVal`/
  `stSeld`) is matched by its own expected-vs-actual type (`memberLeafWireTypes`/
  `IedModel_dataAttributeTypeMatchesMmsType`) only when that uniquely identifies one remaining
  wire value, otherwise left to positional order — confirmed against real hardware that a
  same-leaf-count-different-order mismatch (undetectable by count alone) silently swaps two
  same-shaped siblings (`stVal`/`t` on one device, `stVal`/`stSeld` on another) if left purely
  positional. Falls back to the raw (non-decomposed) entry only on an outright count mismatch or
  an unresolvable q/t match — never on a same-count reorder.
  **ACSE password auth** applied unconditionally from the first connect, covering every reconnect.
  **`MMS_REPORT_CLIENT_CONNECTION_REJECTED`**: a distinct `MmsReportClientConnState` value fired by
  `supervisorLoop` when `IedConnection_connect` returns `IED_ERROR_CONNECTION_REJECTED` — libiec61850
  collapses every non-success connect outcome (wrong ACSE password, plain TCP refusal/timeout, any
  other AARE-level reject) onto this one code, so it's an honest "didn't connect, for some
  rejection-shaped reason" signal, not a precise "wrong password" diagnosis; retries continue
  unconditionally regardless of this state — diagnostic-only. **Edge-triggered** via
  `connectionRejectedSignaled` on `sMmsReportClientHandle`: fires once per rejection streak, reset on
  the next successful connect — without this a device stuck rejecting every attempt would push an
  identical notification every backoff cycle forever. See `ipc_dispatcher`'s own bullet below for
  where this state surfaces on the wire.
  Proven end-to-end in `integration_tests/mms_report_client/`. Full incident history (rollback
  races, reconnect storms, GI removal-then-reinstatement, three real-hardware bugs) is in
  `CHANGELOG.md`.
- `mms_dataset_manager/` — owns everything about an IED's report DATASETS, split out of
  `mms_report_client/` (which had grown to a 3300-line connection file carrying two unrelated
  jobs). Runs against an already-loaded `ied_model` and an already-connected `IedConnection`
  (**borrowed** — it deliberately never opens an association of its own: an association-specific
  dataset created on a different connection would be unassignable to the RCB being enabled).
  Public boundary: `src/features/mms_dataset_manager/service/mms_dataset_manager_api.h` —
  `_create`/`_destroy`, plus a strictly sequential per-connect-cycle lifecycle:
  `MmsDatasetManager_beginCycle` (discovery + budgets + cluster plan) →
  `_resolveForTarget` (once per RCB) → `_endCycle` (orphan cleanup + session teardown), with
  `_cleanupOnStop` for the graceful-stop path. Not internally synchronized — `mms_report_client`'s
  own supervisor thread is the single caller and satisfies that by construction. Knows nothing
  about enabling RCBs, report handlers, value-diff caching or reconnect supervision.
  `MmsDatasetResolution` carries the resolved reference, which tier produced it, the member list
  for decode-shape reconciliation (NULL and EMPTY mean different things — see its own doc comment),
  and `wasNeeded`, the only signal separating "this spare RCB slot was never needed" from "this RCB
  needed a dataset and couldn't get one".
  **Dynamic dataset creation — whole-device clustering**: for every "Dyn" RCB (SCL declares no
  `datSet`), dataset provisioning is planned device-WIDE, not per-RCB/per-LN — a Dyn RCB's own
  parent LN does not restrict what a dataset assigned to it can report on (a dataset's FCDA
  members are independently addressed, confirmed against `IedConnection_createDataSet`'s own wire
  format, `MmsDatasetManagerUseCases_buildWireMemberReferences`, and this repo's own
  `integration_tests/ied_model/fixtures/breaker1.cid`, whose single `LLN0`-parented dataset already
  spans three different LNs). Every Dyn RCB anywhere on the device is therefore treated as one
  fungible reporting slot in a single combined pool, so a device with lots of nominally-redundant
  spare RCB instances on one LN (e.g. `urcbA01`..`urcbJ01`, previously all just duplicating that
  LN's own tiny dataset) can instead cover the entire device's reportable data — including LDs/LNs
  that have zero RCBs of their own.
  Four-tier resolution order per target (`enableOneTarget`): **1. STATIC** (SCL `datSet`,
  unchanged) → **2. PULL LIVE** (already assigned to *this* RCB, unchanged) — `MmsDatasetManagerNaming_looksLikeOurOwnName`'s
  "dangling reference to a prior connection's own destroyed dataset" rejection only ever applies
  to an *unbuffered* target (an association-scoped name is what's actually destroyed on
  disconnect); a buffered target's own domain/VMD-scoped dataset genuinely persists past a
  connection close, so its own live value here is the expected, common reconnect case, not a
  dangling one — see `CHANGELOG.md` for the real bug this used to be. On success, the reused name
  is registered into this cycle's claim-tracking so proactive orphan cleanup (below) never deletes
  it out from under the RCB actively reusing it that same cycle → **3. ADOPT** — an
  existing, not-yet-claimed dataset already on the server under this target's own LD
  (`MmsDatasetManagerDiscovery_findExistingServerDatasets`, `MmsDatasetManagerDiscovery_adoptUnclaimedDataset`) is reused outright, no `createDataSet`
  call at all — "primarily use existing/foreign datasets, create our own only via necessity," per
  explicit product direction; assignment is non-destructive/shareable, so this applies to *any*
  existing dataset regardless of who created it, not just this client's own. A buffered target
  first checks specifically for a candidate matching its *own* deterministic name
  (`MmsDatasetManagerNaming_buildDatasetName(target->objectReference, true)`) before the general LD-wide scan —
  without this, two buffered targets sharing an LD, each with their own pre-existing leftover
  dataset, could cross-adopt each other's leftover depending on server enumeration order (see
  `CHANGELOG.md`) — **4. SELF-CREATE** —
  only if nothing above worked, `MmsDatasetManagerProvisioning_getOrCreateDataset` resolves this target's own
  whole-device cluster (`MmsDatasetManagerProvisioning_buildWholeDeviceClusterPlan`, computed once per connect cycle):
  `IedModel_getReportableAttributeReferencesForWholeDevice` walks every LD/LN in the model, then
  `MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice` (maxAttributes known — DO-atomic
  bin-packing that may legitimately span several different LNs' leaves in one dataset) or
  `MmsDatasetManagerUseCases_groupReferencesByLn` (maxAttributes unknown — one dataset per LN, unbounded, the safe default
  with no known size bound to combine LNs against) packs it into clusters assigned to Dyn slots in
  simple model-declaration order; whichever list (clusters or slots) runs out first is logged
  plainly, never silently dropped. **Unbuffered**: association-scoped (`@`-prefixed, auto-destroyed
  on disconnect, no cleanup needed). **Buffered**: domain/VMD-scoped (`"$"`-joined, no `@` prefix,
  persists past the connection) instead — an association-scoped dataset is destroyed the instant
  the connection closes, which a real device (and the vendored reference server) rejects assigning
  to a buffered RCB outright (`IED_ERROR_OBJECT_VALUE_INVALID`/error 32 — confirmed against a real
  SIPROTEC 6MD device, see `CHANGELOG.md`). A later `createDataSet` attempt for the same
  (deterministic, per-target) name legitimately fails with `IED_ERROR_OBJECT_EXISTS` — treated as a
  successful reuse, not an error. **Once that domain-scoped fallback has actually worked once in a
  connect cycle, the association-specific attempt is skipped for every remaining unbuffered target**
  (`MmsDatasetManagerSession.associationSpecificCreateRejected`): on a device that rejects the form
  structurally it never succeeds for any target, so retrying per-target only spends a doomed
  ~100-member round-trip each time (nine wasted large PDUs in one real connect cycle). Latched only
  on a fallback that succeeded — if both scopes fail, the association-specific form isn't
  demonstrably the problem — and per-cycle only, never persisted.
  **Real budget, not a blind guess**: `MmsDatasetManagerDiscovery_findExistingServerDatasets` (once per connect cycle,
  scoped to Dyn targets' own LDs, via `IedConnection_getLogicalDeviceDataSets`) feeds
  `MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget`, correcting SCL's declared
  `<Services><DynDataSet max="N" maxAttributes="M"/>` for datasets that already exist server-side
  (leftover domain-scoped ones from an earlier ungracefully-terminated run, another tool's own
  datasets, etc.) — confirmed on a real device that a naive blind-reset counter believed budget
  remained while a pile of leftovers had already exhausted the real one. A discovered name already
  known to be SCL-claimed by a *different* target's own static `datSet` is excluded from the
  adoption pool (never redundantly adopted instead of covering new device data).
  **Proactive orphan cleanup** (`MmsDatasetManagerProvisioning_cleanupOrphaned`, run from
  `MmsDatasetManager_endCycle`): any domain-scoped dataset that exactly reconstructs via
  `MmsDatasetManagerNaming_buildDatasetName(target->objectReference,
  true)` for some real buffered Dyn target right now, but wasn't claimed (adopted or
  self-created/reused) by any target this cycle, is deleted to reclaim budget — closing the
  ungraceful-restart gap `MmsDatasetManager_cleanupOnStop` can't reach (a
  killed/crashed daemon never gets there). Strict, conservative match only — never a name that
  merely looks like it might be ours, and a foreign dataset is never deleted, only ever adopted.
  the dataset manager's own `domainScopedDynamicDatasetNames` still tracks every domain-scoped name this client
  itself created or adopted, for `MmsDatasetManager_cleanupOnStop`'s own graceful-stop deletion pass
  (unchanged, the other half of this lifecycle).
  Entirely inert (identical to pre-existing per-LN behavior) whenever SCL doesn't declare
  `<DynDataSet>` at all, beyond the discovery/adoption/cleanup passes, which apply regardless. The
  no-SCL empirical/adaptive-discovery case (*inferring* an unknown `maxAttributes`/`max` cap from
  `createDataSet` failure patterns, as opposed to *discovering already-existing datasets*, which is
  now implemented) remains unimplemented — deferred, see `GAP3_DYNAMIC_DATASET_NOTES.md`.
- `goose_subscriber/` — subscribes to every GOOSE Control Block on one IED via `ied_model`
  (never re-parses SCL, never discovers GoCBs over the wire), applying
  `GooseSubscriber_setDstMac`/`setAppId` filters from SCL addressing when present, and delivers
  normalized `GooseSubscriberRecord`s via a caller-registered callback. GOOSE never carries a
  reference on the wire, so `GooseSubscriberEntry.reference` is always resolved via a per-target
  cache of `IedModel_getDataSetMemberReferences`. Works under every `AccessMode`. Public boundary:
  `src/features/goose_subscriber/service/goose_subscriber_api.h` — public functions are named
  `GooseSubscription_*`, not `GooseSubscriber_*` (libiec61850's own equivalents would otherwise
  collide). GOOSE is connectionless, so there's no reconnect-supervisor thread — instead a single
  low-rate **liveness-polling** thread watches `GooseSubscriber_isValid()` per target and reports
  `VALID`/`STALE`/`INVALID_STATE` transitions (the one narrow exception to the "No cyclic polling"
  Hard Rule). Mirrors `mms_report_client`'s event-driven filtering exactly — GOOSE has no
  `ReasonForInclusion` concept, so its `cached == NULL` bootstrap suppression is the entire
  mechanism suppressing both the first-ever frame per target and the first frame after a liveness
  recovery. Also skips an ordinary `MinTime`/`MaxTime` heartbeat retransmission via a per-target
  `hasForwardedStNum`/`lastForwardedStNum` pair, reset on STALE/INVALID_STATE→VALID. Proven
  end-to-end against a real `ied_simulator` IED publishing real GOOSE over `lo` in
  `integration_tests/goose_subscriber/` (needs `sudo`).
- `src/orchestration/` — sequences the features above for **one IED**: `ipc_dispatcher` (bind
  first, fail-fast) → `scl_bootstrap` (probe hosts, fetch SCL bytes) → stage to a temp file →
  (if `iedName` empty: auto-detect via `IedModel_listIedNames` — exactly one `<IED>` used
  silently, zero/more-than-one fails hard) → `ied_model` (load) → `mms_report_client` (start;
  wired to `IpcDispatcher_onMmsReport`) → `goose_subscriber` (start; wired to
  `IpcDispatcher_onGooseRecord`). Public boundary: `src/orchestration/service/orchestration_api.h`.
  Not itself a "feature." `Orchestration_run` is one blocking call. **Fail-hard**: if any stage
  fails, everything started by earlier stages is torn down (reverse order), leaving the handle
  re-runnable. Single-IED scope.
  **`Orchestration_runFromLocalFile(handle, sclFilePath, host, mmsPort, iedName, interfaceId,
  accessMode, outDetail)`** is a second entry point, for devices with no file services — shares
  the continuation via a private `runFromSclFile` helper; only stage 0-1 differs.
  **`Orchestration_runFromOnlineDiscovery(handle, host, mmsPort, iedName, interfaceId, accessMode,
  acseAuthPassword, outDetail)`** is a third, for devices with no file services and no local SCL
  file — calls `ied_model_online_loader`'s `IedModelOnlineLoader_build`, joining the same shared
  tail. **Never** invoked automatically inside `Orchestration_run` — `device_manager`'s bootstrap
  policy calls it explicitly, only as a one-shot retry after `Orchestration_run` fails with
  exactly `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND`. See "No over-the-wire tree discovery" Hard
  Rule. **`Orchestration_wireConnStatusToIpcDispatcher(handle)`** wires the connState slot
  (otherwise caller-supplied via `Orchestration_setReportConnStateCallback`, mutually exclusive
  with it) to `ipc_dispatcher`'s own `IpcDispatcher_onConnStateChange` — see that feature's own
  bullet below for what it does with it. Must be called before `Orchestration_run*`, same as every
  other setter. Proven end-to-end against a real `ied_simulator` IED in
  `integration_tests/orchestration/` (needs `sudo`), including a device that accepts one
  `acseAuthPassword` for SCL fetch but rejects a different one on the report client's own
  association — `Orchestration_run` still returns `ORCHESTRATION_OK` in that case (by design: the
  report client's own reconnect loop is what surfaces the problem, not `Orchestration_run` itself),
  so `test_authRequired_bootstrapSucceedsButReportClientRejected_deliversConnectionStatusPush`
  asserts the `CONNECTION_STATUS` push arrives instead.
- `ipc_dispatcher/` — relays normalized `MmsReportRecord`/`GooseSubscriberRecord` data out over a
  websocket, hosted internally only (`127.0.0.1`, no TLS). Push-only. Public boundary:
  `src/features/ipc_dispatcher/service/ipc_dispatcher_api.h`. Its MMS/GOOSE callback-adapters are
  registered by `src/orchestration/` itself unconditionally — no way to register a second
  consumer (each record's ownership transfers to the one registered consumer, which destroys it).
  **`ipc_dispatcher_conn_state_adapter`** is a third adapter, `IpcDispatcher_onConnStateChange`,
  but unlike the other two it's opt-in, not automatic: `orchestration_api.h`'s own
  `connStateCallback` slot is caller-supplied in general, so
  `Orchestration_wireConnStatusToIpcDispatcher(handle)` exists purely as a convenience wrapper
  routing it here without the caller needing to depend on `ipc_dispatcher`'s header directly —
  `device_manager_api.c` calls it before every `Orchestration_run*`. Pushes a `CONNECTION_STATUS`
  message (`{schemaVersion, type: "CONNECTION_STATUS", status: "CONNECTION_REJECTED"}`) only for
  `MMS_REPORT_CLIENT_CONNECTION_REJECTED` — every other `MmsReportClientConnState` is a no-op, since
  this stream isn't meant to surface routine `CONNECTING`/`CONNECTED`/backoff churn, only the one
  diagnostic state a caller can act on (see `mms_report_client`'s own bullet above).
  **Quality (`q`) pairing**: `IpcDispatcherUseCases_pairQuality` walks up each value's ancestor
  prefixes one `$`-segment at a time (not a single last-`$` strip — required for deeply nested
  CONSTRUCTED-DA chains, e.g. a CMV's `q` several segments above its terminal leaf) to find its
  sibling; a lone `q` is dropped. Quality validity decodes into a named 4-value enum; remaining
  bits copy verbatim into a raw `uint16_t` passthrough field.
  **Value codec**: scalar types map directly; `MMS_BIT_STRING` maps to a raw unsigned integer
  (covers CODEDENUM DAs like `Dbpos`/`Tcmd`) — never a decoded semantic label, since this function
  can't know which specific CODEDENUM a bitstring represents (an earlier SCL-derived label field
  was added, then removed at explicit user request — see `CHANGELOG.md`). Anything else falls
  back to an owned `"<unsupported:...>"` placeholder.
  **Threading**: producer threads (`mms_report_client`'s supervisor, `goose_subscriber`'s
  reception thread) must never block, while libwebsockets requires all `lws_write`/context access
  on the `lws_service()` thread — so each adapter only serializes to JSON, pushes it onto a
  bounded, mutex-guarded broadcast ring, then wakes the lws thread (the only libwebsockets call a
  producer thread makes). The service-loop thread drains the ring per-connection via a read
  cursor; a lagging client's cursor jumps forward and drops unseen messages (no backlog replay).
  **JSON envelope** (stable contract): `{schemaVersion, type: "MMS_REPORT"|"GOOSE", source: {...},
  hasTimestamp, timestampMs?, dataPoints: [{reference, value, quality, previousValue,
  previousQuality, label, previousLabel}]}` — `dataPoints` only contains points that actually
  changed (see "IPC / Reporting Out"). Proven end-to-end (real bind, a hand-rolled minimal RFC6455
  test client) in `integration_tests/ipc_dispatcher/` — no `sudo` needed.
- `ied_discovery/` — finds candidate IEC 61850 MMS devices on the local network instead of
  requiring an operator to already know a target IP. Not part of `orchestration`'s own sequence —
  driven by `scan_orchestration`'s worker only. Two-stage verification per candidate: (1) a cheap
  bounded-concurrency TCP probe, reusing `scl_bootstrap`'s own async-probe machinery via
  `SclBootstrap_tcpProbeOnly`; (2) for TCP survivors only, a real MMS/ACSE association
  (immediately closed, no file browsing/SCL fetch — deferred to `scl_bootstrap`'s later run). Only
  a host passing both stages counts as confirmed — host discovery on an already-named local
  interface (`getifaddrs()` + CIDR math), not the "no over-the-wire tree discovery" Hard Rule
  (this never touches SCL/the data model). Public boundary:
  `src/features/ied_discovery/service/ied_discovery_api.h`: `IedDiscovery_scanSubnet` (enumerate +
  verify every host on a subnet, capped by `IedDiscoveryConfig.maxHosts`) and
  `IedDiscovery_verifyHost` (verify one host). Optionally gated behind ACSE password auth (one
  retry) via `IedDiscoveryConfig.acseAuthPassword`. Driven purely by `scan_orchestration`'s worker.
  **Address selection matters and is not "the interface's address"**: an interface routinely
  carries more than one IPv4 address here — every deployed box permanently holds a fixed
  `169.254.1.1/24` recovery address next to its real static IP (the parent repo's
  `deploy/setup.sh`), and the kernel lists the link-local one *first*.
  `IedDiscoveryNetif_getInterfaceIpv4` therefore prefers the first non-link-local address
  (`IedDiscoveryCidr_isLinkLocal`), falling back to a link-local one only when it is the
  interface's only address; loopback is deliberately still selectable, since `lo`'s `/8` being
  rejected as `SUBNET_TOO_LARGE` is itself an asserted behavior. Taking the first address instead
  silently swept `169.254.1.0/24` and confirmed nothing, with no error — see `CHANGELOG.md`.
  `IedDiscovery_scanSubnet` prints one `[scan] sweeping <network>/<prefix> on <iface> (N hosts)`
  line per sweep (the only logging in this feature) precisely because that failure was otherwise
  indistinguishable from an empty network.
  **A host whose real MMS/ACSE association is specifically access-denied (not merely
  non-responsive/non-MMS) is still a discovered device, not a dropped one**: `IedDiscoveryMmsProbe_associate`
  reports this distinction via an `outAccessDenied` out-param (previously computed internally then
  discarded — a real password-protected IED with no/wrong credentials configured was
  indistinguishable from nothing being at that address at all, silently vanishing from every scan).
  `IedDiscoveryHostStatus` now has a distinct `IED_DISCOVERY_HOST_ACCESS_DENIED` value (`verifyHost`'s
  return), and `IedDiscovery_scanSubnet` returns a `LinkedList` of owned `IedDiscoveredHost{host,
  authRequired}` (not bare `char*` IPs) — a host is included if it either fully associated or was
  access-denied. Caller owns the list via `IedDiscovery_destroyHostList`, not a bare
  `LinkedList_destroyDeep(list, free)`. Proven end-to-end (`verifyHost` against a real
  `ied_simulator` IED, including the ACSE-auth-retry symmetry and the access-denied/no-password and
  wrong-password cases) in `integration_tests/ied_discovery/` — no `sudo` needed. A real subnet scan
  is machine-topology-dependent and not automatable; see that E2E test's Commands bullet for the one
  deterministic `getifaddrs` case covered.
- `ied_model_online_loader/` — builds a complete `IedModelHandle` by walking a live IED's own MMS
  ACSI directory/model-discovery services, for devices that associate fine but never serve an SCL
  file (confirmed against a real OMICRON IED Scout "Simulate IED" instance). The **one** narrow,
  deliberate exception to the "No over-the-wire tree discovery" Hard Rule — every call is a
  `libiec61850` client API call, and it only runs as an explicit fallback
  (`Orchestration_runFromOnlineDiscovery`), never silently instead of SCL parsing. Public
  boundary: `src/features/ied_model_online_loader/service/ied_model_online_loader_api.h`, one
  entry point: `IedModelOnlineLoader_build(host, port, iedName, mode, acseAuthPassword, config,
  outError)` — owns its own one-shot `IedConnection`. Walks `IEC61850_CLIENT_MODEL_DISCOVERY` (LD
  list → LN directory → per-LN control-block enumeration by ACSI class + DO/DA tree at FC=ST/MX →
  RCB/GoCB values → dataset member lists) to build a real dynamic `IedModel*` (same construction
  calls `ied_simulator` uses — `IedModel_create("")` since `LogicalDevice_create` implicitly
  prepends the parent's name), then wraps it via `IedModel_wrapDynamicModel(model, iedName,
  mode)` — every existing `ied_model` accessor then behaves identically regardless of origin.
  `IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef` converts ACSI references to this
  codebase's `"$"`-joined wire form, splitting the `"LD/LN"` prefix and keeping only the LN
  portion — using the whole unsplit prefix silently broke Gap-4 decomposition for
  online-discovered DO-level members (see `CHANGELOG.md`).
  **Known v1 limitations**: only builds FC=ST/MX structure; `DataAttributeType` is a coarse MMS
  wire-type mapping; GoCB addressing counts as "populated" only if not all-zero (degrades safely);
  no dataset-count/`maxAttributes`-cap handling; slower than one SCL transfer + local parse.
  Proven end-to-end against a real `ied_simulator` IED with an empty fixture directory in
  `integration_tests/ied_model_online_loader/` — no `sudo` needed.
- `scan_dispatcher/` — a near-verbatim structural duplicate of `ipc_dispatcher`'s ring-buffer +
  libwebsockets-service-thread transport (own port, default **8766**, vs. `ipc_dispatcher`'s
  8765), relaying "device found" scan events. Deliberately **duplicated, not shared** — no
  precedent for a cross-feature "shared" directory. Public boundary:
  `src/features/scan_dispatcher/service/scan_dispatcher_api.h` — create/start/stop/destroy mirror
  `ipc_dispatcher`'s contract, plus one typed publish entry point,
  `ScanDispatcher_publishDeviceFound(handle, scanId, host, mmsPort, authRequired)`. Has **no
  knowledge of scans, interfaces, or reference-counting** — purely transport; `scan_orchestration`
  decides when to start/stop it. JSON envelope: `{schemaVersion, type: "SCAN_RESULT", scanId, host,
  mmsPort, discoveredAtMs, authRequired}` — `authRequired: true` means `ied_discovery` classified
  this host as access-denied (a real device needing credentials) rather than fully associated; still
  additive under `schemaVersion: 1`, no version bump. Proven end-to-end in
  `integration_tests/scan_dispatcher/` — no `sudo` needed.
- `src/scan_orchestration/` — a top-level sibling of `src/features/`, sequencing `ied_discovery`
  (left entirely untouched) and `scan_dispatcher` into a continuous, background,
  reference-counted, multi-scan-capable service. Public boundary:
  `src/scan_orchestration/service/scan_orchestration_api.h` — `_create`/`_destroy`,
  `_setDeviceFoundCallback`, `ScanOrchestration_startScan(handle, request, outScanId)` /
  `_stopScan(handle, scanId)`, and `_snapshotDiscoveredHosts` (unused today — every client gets
  results over `scan_dispatcher`'s websocket instead). **Per-scan worker**: owns a private
  `IedDiscoveryHandle` and its own seen-set, looping sweep → diff → publish only genuinely new
  hosts → interruptible sleep → repeat. The worker sleeps a fixed
  `SCAN_ORCHESTRATION_INITIAL_SWEEP_GRACE_MS` (300ms) before its very first sweep — `scan_dispatcher`
  only binds on this scan's own 0→1 transition, so a client can't have a live connection to it
  before `START_SCAN`'s ack arrives and only reconnects after receiving that ack; both dispatcher
  websockets use "start-from-now" ring-buffer cursors with no backlog replay, so a host found by
  the first sweep before that reconnect lands is lost to that client forever (seen-set dedup means
  it's never republished on a later sweep either). The grace delay only runs once, before the
  first sweep. **Known limitation**: stop cannot interrupt an in-flight
  sweep — worst case several seconds for a /24 at default config. **Registry** is two-phase
  locked: a short critical section reserves scanIds and does the 0→1 `ScanDispatcher_start`
  together, but removal hands the potentially-slow stop/destroy back to the caller entirely
  outside any lock — otherwise one scan's slow stop would serialize every other scan behind it.
  Proven end-to-end (real start/stop against `lo` with two `mmsPort`s, the full
  0→1→2→1→0→1-rebind refcounting cycle) in `integration_tests/scan_orchestration/` — no `sudo`
  needed. Sweeps against `lo` are expected to fail (`/8` exceeds `maxHosts`) and are tolerated
  gracefully — this test proves sequencing, not sweep success.
- `src/device_manager/` — a top-level sibling of `src/features/`/`src/orchestration/`/
  `src/scan_orchestration/`, runs SEVERAL `orchestration` pipelines concurrently, one per physical
  IED, each auto-assigned its own `ipc_dispatcher` port, addressable by a server-generated
  `deviceId`. The feature layer itself has no global/static state — this layer supplies the
  missing registry, `control_dispatcher` supplies the control-plane transport. A
  synchronous library, no thread of its own — start/stop block the calling thread as long as the
  underlying `Orchestration_run*`/`_stop` call takes, but never serialize behind a DIFFERENT
  device's own slow call. Public boundary: `src/device_manager/service/device_manager_api.h` —
  `DeviceManager_startReporting(handle, host, mmsPort, iedName, interfaceId, sclFilePath,
  acseAuthPassword, accessMode, outDeviceId, outWsPort, outDetail)` /
  `DeviceManager_stopReporting(handle, deviceId, outDetail)`. `iedName` is mandatory whenever
  `sclFilePath` is given. The bootstrap policy holds the one shared "start a device" sequencing
  function (local file vs. `scl_bootstrap` with a one-shot online-discovery retry). Port
  allocation is a `[9000,9999]`-default range with a free-list. The registry is three-phase
  locked, same shape as `scan_orchestration`'s own. `acseAuthPassword` is `strdup`'d into the
  registry entry, since a control-plane message's source buffer is freed once the request
  finishes (unlike `main.c`'s own argv). A duplicate `StartReporting` for a running/mid-start
  `(host, mmsPort)` is rejected (`DeviceManagerRegistry_reserve` →
  `DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING`); `lnCategories` is deliberately **not** part of that
  key. **One daemon device per physical IED is a correctness requirement, not a simplification** —
  the obvious-looking idea of running two devices against one IED with disjoint LN categories (so
  two operators can each watch "their" part) is unsafe, for four independent reasons, and the
  rejection above is what prevents all of them:
  **(1) Categories don't partition the work.** `IedModelUseCases_getReportSubscriptionTargets`
  deliberately does not gate RCB (or GoCB) visibility by category — real SCL parents nearly every
  RCB on `LLN0`, so gating there would make the filter useless. Both clients therefore enumerate
  every RCB and every GoCB on the device; the filter only trims data points at delivery and
  cluster membership at dataset-planning time.
  **(2) Dataset names collide.** `MmsDatasetManagerNaming_buildDatasetName` derives a buffered
  name from the RCB's LN reference alone, with no client identity, while the *contents* come from
  a category-filtered whole-device plan. Two clients derive identical names for different intended
  member lists, and `MmsDatasetManagerProvisioning_getOrCreateDataset` treats
  `IED_ERROR_OBJECT_EXISTS` as successful reuse — so the second client silently reports the
  first's category set and reconciles its decode-shape cache against a plan it never made.
  **(3) Orphan cleanup deletes the other client's live datasets.**
  `MmsDatasetManagerProvisioning_cleanupOrphaned` decides "is this ours?" purely by reconstructing
  the name for one of its own targets, against per-connection claim state. Anything the other
  client created and is actively reporting through reconstructs as a valid unclaimed name, so it
  gets `RptEna=false`, `DatSet=""`, and `deleteDataSet` — server-side, with no local error on the
  victim, which only finds out on its next reconnect. `_cleanupOnStop` has the same shape.
  **(4) Shared BRCBs thrash.** Step 1's `DatSet` write clears `TrgOps` *and* `OptFlds` on real
  SIPROTEC hardware, so each client's enable wipes the other's `dchg|qchg|gi` and
  `RPT_OPT_ENTRY_ID` — the exact silent-`TrgOps=0x0` failure documented under `mms_report_client`.
  A BRCB is also single-owner per IEC 61850-7-2 (`Resv`/`ResvTms`/`ClientLN`), `<DynDataSet max>`
  budget is shared (starvation, then a create/delete oscillation as each client's cleanup reclaims
  the other's), and `EntryID` is one server-side attribute two clients would rewind against each
  other. Only `goose_subscriber` and `ipc_dispatcher` port allocation are genuinely unaffected.
  Real per-client isolation would need client identity in dataset names, RCB-slot reservation,
  partitioned budgets, and cleanup scoped by client — none of which exists. Narrowing what a given
  operator *sees* is a client-side concern instead: `ipc_dispatcher` stamps every data point with
  its own `category`, so the API/frontend filter one shared stream per viewer.
  `DeviceManager_stopReportingByAddress(handle, host, mmsPort, outDeviceId, outDetail)` is a
  second stop entry point, address-keyed instead of deviceId-keyed — for a caller that never
  obtained or has lost track of a deviceId (its own `StartReporting` call raced/timed out, or its
  own bookkeeping was lost, e.g. a process restart). Resolves the address to a deviceId via
  `DeviceManagerRegistry_findDeviceIdByHost` (same match scope, running or mid-start, as the
  dedupe check `reserve()` already does), then delegates to `DeviceManager_stopReporting`
  unchanged — every existing behavior (mid-start's `DEVICE_MANAGER_ERR_START_IN_PROGRESS`, atomic
  remove against a concurrent duplicate stop) applies identically; this is purely an alternate
  key, not a different stop path. `DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND` if nothing is registered
  at that address.
  Proven end-to-end (two real `ied_simulator` instances, two concurrent starts, distinct
  deviceIds/ports each streaming real GOOSE JSON) in `integration_tests/device_manager/` — needs
  `sudo`. **Does NOT watch connection health or auto-stop a device on connection loss** — only an
  explicit `STOP_REPORTING` tears one down (a reaper was built, then removed again at explicit
  user request — see `CHANGELOG.md`).
- `control_dispatcher/` — the first **bidirectional** websocket surface in this codebase: unlike
  `ipc_dispatcher`/`scan_dispatcher` (push-only), this one RECEIVES JSON commands
  (`START_REPORTING`/`STOP_REPORTING`/`START_SCAN`/`STOP_SCAN`) over its one well-known websocket
  port (default **8767**) and pushes back JSON acks/errors, relaying the first pair to
  `device_manager` and the second pair to `scan_orchestration`. Public boundary:
  `src/features/control_dispatcher/service/control_dispatcher_api.h` —
  `ControlDispatcher_create(config, deviceManager, scanOrchestration, outError)` / `_start` /
  `_stop` / `_destroy`. This is the daemon's **only** interface. The first `cJSON_Parse` call in
  this codebase's production code — every field is defensively type/`NULL`-checked (untrusted
  network input).
  **Threading**: the lws service thread accumulates `LWS_CALLBACK_RECEIVE` fragments into a
  bounded (8KB) per-session buffer, parses the final fragment — a parse/validation failure or full
  request queue is handled entirely on this thread; a successfully queued request instead calls a
  function-pointer callback into a dedicated worker thread, which runs the slow call off the lws
  thread, then pushes the JSON result onto the ring buffer and wakes the lws thread (the only
  libwebsockets call a producer thread may make directly). The request queue is a bounded FIFO
  with its own mutex, separate from the ring buffer's lock — full queue returns `SERVER_BUSY`
  directly from the lws thread.
  **JSON envelope** (stable contract): inbound `{requestId, action:
  "START_REPORTING"|"STOP_REPORTING"|"START_SCAN"|"STOP_SCAN", params: {...}}` — see this file's
  own `main.c` Commands bullets for each action's shape. `START_SCAN`/`STOP_SCAN` deliberately
  have **no** `acseAuthPassword` field. Outbound: `{schemaVersion, requestId, action, success,
  result, error}` — `error: {code, message, stage?, detail?}`, `code` mirroring
  `DeviceManagerError`/`ScanOrchestrationError` plus shared parse-side codes.
  **Fan-out is broadcast to every connected control client** — a real tradeoff if the
  "loopback-only, one real client" trust assumption ever changes. **No unsolicited
  `DEVICE_STOPPED` push exists** — a device only leaves the registry via an explicit
  `STOP_REPORTING` round trip (an earlier revision pushed one, tied to `device_manager`'s
  connection-health monitor; both were removed together — see `CHANGELOG.md`).
  Proven end-to-end (real bind, a hand-rolled RFC6455 client sending masked command frames and
  receiving responses; real `START_REPORTING`/`STOP_REPORTING` against a real `ied_simulator`
  instance; real `START_SCAN`/`STOP_SCAN` over `lo`, no `sudo`) in
  `integration_tests/control_dispatcher/` — needs `sudo` only for the real-device case.

## The Two Workers
- **GOOSE Sniffer** — `GooseReceiver`/`GooseSubscriber` (see `third_party/include/goose_receiver.h`,
  `goose_subscriber.h`) via `GooseReceiver_start()`'s library-managed raw-socket reception
  thread — not libpcap/Npcap (libiec61850's own `hal_ethernet` PAL provides the raw
  AF_PACKET-style socket directly; no separate capture library is used or vendored). Event-driven
  on frame arrival, never polled for *reception*. Implemented in `src/features/goose_subscriber/`.
- **MMS Report Client** — `IedConnection` (see `iec61850_client.h` for the client-side `ClientReportControlBlock`/`ClientReport`/`ReportCallbackFunction` API; `reporting.h` is the *server*-side implementation, not what a client uses), driven entirely by BRCB/URCB. Never poll for data. Implemented in `src/features/mms_report_client/`.
- Both workers normalize output to a common JSON report shape before it reaches IPC.

## Hard Rules (with reasons)
- **libiec61850 is mandatory** for all protocol handling — never hand-roll GOOSE or MMS parsing; this is a correctness and safety liability in this domain.
- **No cyclic polling**, on either worker — polling defeats the reporter's whole reason for existing (event-driven, low-latency). **One narrow, deliberate exception**: `goose_subscriber`'s liveness thread polls `GooseSubscriber_isValid()` at a low rate purely to detect *staleness* (a publisher going silent) — GOOSE is connectionless and has no push signal for that. It never gates or delays actual record delivery, which stays 100% event-driven via `GooseListener`; it only detects the absence of frames, structurally unobservable any other way in a connectionless protocol.
- **No over-the-wire tree discovery.** Parse `.scd`/`.icd` at boot to know what to subscribe to — runtime discovery is slow and fragile against flaky IEDs. **One narrow, deliberate exception**: `ied_model_online_loader` walks a live server's MMS ACSI directory services to reconstruct an equivalent `IedModel` purely as a **fallback**, engaged only via `Orchestration_runFromOnlineDiscovery` when `scl_bootstrap` has already failed with `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` (a connectable IED — e.g. OMICRON IED Scout's "Simulate IED" mode — that never serves an SCL file). Never a silent, automatic substitute inside `Orchestration_run` itself — without it, a device with no file services is entirely unreportable, full stop. See that feature's own Architecture bullet. GOOSE addressing precision is also degraded when this fallback is used: SCL almost always carries an explicit `<GSE><Address>`, filtering tightly at the socket level; discovery only gets this if the device exposes the optional `DstAddress` GoCB attribute, and reception still works without it (matches on `gocbRef` in the frame) but the NIC sees more traffic before software filtering.
- **No dangling connections.** Explicit pooling, keep-alives, exponential backoff on MMS — IEDs drop connections under load.
- **Don't touch `third_party/`** — pre-built and vendored; if headers seem to be missing something, say so, don't hand-edit. `third_party_src/` is a separate directory of git submodules (`cJSON`/`libiec61850`/`libwebsockets`/`mxml`/`Unity`, each pinned to a specific upstream commit, not checked out by default until `git submodule update --init`), used only by `setup_project.sh` to regenerate `third_party/` if ever genuinely needed — the normal build never touches it either, but it's the one place hand-editing/rebuilding from source is the point, not a violation of this rule.
- **Don't add dependencies without asking** — dependency surface is deliberately minimal.
- **If unsure of exact IEC 61850 semantics** (FC codes, DA types, BRCB trigger options), say so and cite the spec section or the relevant `third_party/include` header — don't guess.

## IPC / Reporting Out
- Normalize C structs (GOOSE frame, MMS report) to JSON, dispatched via a websocket
  (`ipc_dispatcher/`, loopback-only) to the high-level API and frontend — the "reporting"
  surface; treat message shape as a stable contract, flag breaking changes explicitly. See
  `ipc_dispatcher/`'s own Architecture bullet for the full envelope shape and threading design.
- **Target consumer needs four fields per data point: value, reference, quality, timestamp**
  (timestamp lowest priority, still wanted). `ipc_dispatcher` assembles all four into one JSON
  data point per value.
- **Field availability** (`MmsReportEntry`/`GooseSubscriberEntry`): **value** present on both.
  **reference** populated on both via `IedModel_getDataSetMemberReferences`, purely local — MMS
  prefers the server's own `ClientReport_getDataReference` (if `OptFlds.DataRef`), falling back
  to the SCL-derived reference; GOOSE always uses the SCL-derived reference. **quality** is not a
  field on either struct, inherently: `q` is a sibling Data Attribute of `stVal`, present only if
  the SCL dataset includes a `q` entry alongside `stVal` — when present, `ipc_dispatcher` pairs it
  with its value sibling. **timestamp** is one shared, record-level value, not per-entry — MMS
  only if `hasTimestamp`, GOOSE always (frame's publish time).
- **The websocket output is a pure stream of *changes***, at explicit user request: the genuine
  first-ever GI/bootstrap snapshot (and GOOSE's first-ever frame per target) never reaches the
  websocket — cache-seed-only, and a point that never changes again never reaches the frontend
  (explicit, accepted tradeoff). This applies ONLY to the genuine first-ever observation — the
  value-diff cache is never reset afterward, so a reconnect's/recovery's fresh snapshot diffs
  against the real, preserved last-known value: a genuine change made while disconnected/stale
  reaches the websocket with a real `previousValue`. `dataPoints` only ever contains points that
  actually changed, judged at the (value, quality) *pair* level — a dragged-along sibling's own
  `previousValue` legitimately equals its current `value`. Every forwarded point carries
  `previousValue`/`previousQuality`, `NULL`/absent only where a wire position has no cache slot.

## Interaction Style
- No fluff, no filler. Peer-to-peer register.
- Use opaque pointers / forward declaration to enforce API boundaries.

## Output Format
- Small fixes: just the diff/code, no ceremony.
- New features/architectural changes: (1) where it fits in feature-first layout, (2) the code, (3) Watch Out — link order, memory safety, thread risk.
