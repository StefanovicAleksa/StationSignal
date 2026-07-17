# src/orchestration/

> **Not a feature.** Per `CLAUDE.md`'s Architecture section, `src/orchestration/` is a top-level
> sibling of `src/features/` — one of three sequencing layers (`src/orchestration/`,
> `src/scan_orchestration/`, `src/device_manager/`) alongside the nine `src/features/<name>/`
> directories. It contains no protocol logic, no third-party includes of its own, and no business
> rules beyond call ordering and rollback. Its entire job is wiring six already-implemented
> features together, in order, **for one IED**. Multi-IED fan-out is `src/device_manager/`'s job,
> layered on top of this.

## 1. Overview

`src/orchestration/` sequences, for a single physical IED: `ipc_dispatcher` (bind + start its
websocket first, deliberately, so a bind failure fails fast before any network-facing MMS/GOOSE
work happens) → `scl_bootstrap` (probe a host list, fetch SCL bytes over MMS file services) →
stage those bytes to a `mkstemp` temp file → optional IED-name auto-detection (only when the
caller didn't supply one) → `ied_model` (parse the staged/local file into a queryable model) →
`mms_report_client` (create + start against the winning host, report callback unconditionally
wired to `ipc_dispatcher`) → `goose_subscriber` (create + start on `interfaceId`, same
unconditional `ipc_dispatcher` wiring). It exists because nothing upstream of it (`main.c`,
`device_manager`) should have to know the seven-stage internal ordering, the fail-hard rollback
rules, or which two features' data-record callbacks are hardwired rather than caller-settable —
that knowledge lives in exactly one place.

There are **three entry points**, all funneling into the same shared tail once a model exists:

- **`Orchestration_run`** — the common case: fetch SCL over the wire via `scl_bootstrap`, then
  stage/parse/connect.
- **`Orchestration_runFromLocalFile`** — same end state, but skips `scl_bootstrap`/staging
  entirely; reads a caller-supplied local SCL file directly. For IEDs whose MMS server has no
  file services but whose SCL is available some other way (e.g. exported from the tool driving a
  simulation).
- **`Orchestration_runFromOnlineDiscovery`** — a third way to obtain a model, for IEDs with
  neither file services nor a locally available SCL file: walks the live device's own MMS ACSI
  directory services via `ied_model_online_loader` instead of parsing SCL at all. Deliberately
  never invoked automatically inside `Orchestration_run` — see §6.

Public boundary: `src/orchestration/service/orchestration_api.h`. Callers (`main.c`,
`device_manager`) must only ever include this header — never reach into `domain/`/`data/`/`utils/`
directly, and never include `ipc_dispatcher`'s own service header either, since this layer owns
`ipc_dispatcher`'s entire lifecycle (create/start/stop/destroy, plus wiring its callbacks onto
`mms_report_client`/`goose_subscriber`) end to end.

## 2. Public API surface

All declared in `src/orchestration/service/orchestration_api.h`.

- **`void OrchestrationConfig_defaults(OrchestrationConfig* config)`** — fills in each embedded
  sub-config's own defaults (`IpcDispatcherConfig_defaults`, `SclBootstrapConfig_defaults`,
  `MmsReportClientConfig_defaults`, `GooseSubscriberConfig_defaults`). NULL-safe no-op.

- **`OrchestrationHandle Orchestration_create(const OrchestrationConfig* config, OrchestrationError* outError)`**
  — allocates only, no I/O — matches every sibling feature's own "no I/O at create" contract, with
  one deliberate exception: `IpcDispatcher_create` is called here too (its own ring-buffer
  allocation), but `IpcDispatcher_create` itself never binds a socket — the real bind happens
  later, at `Orchestration_run`'s (or either sibling entry point's) first stage. `config == NULL`
  means `OrchestrationConfig_defaults`. Returns NULL + `ORCHESTRATION_ERR_OUT_OF_MEMORY` on
  `calloc`/`IpcDispatcher_create` failure.

- **Diagnostic callback setters** — `Orchestration_setReportConnStateCallback`,
  `Orchestration_setRcbStatusCallback`, `Orchestration_setGooseStatusCallback`,
  `Orchestration_setBootstrapProgressCallback`. Must be called before any `Orchestration_run*`
  call — mirrors `MmsReportClient`/`GooseSubscription`'s own "setters read only at `_start()`"
  contract: `Orchestration_run*` registers each stored callback on the underlying sub-feature
  handle right before that sub-feature's own `_start()`. Pure passthrough, no wrapping, no
  orchestration-specific record types — callback typedefs are reused directly from the wrapped
  features. `Orchestration_setBootstrapProgressCallback`'s callback fires **synchronously on the
  calling thread**, during the (potentially slow) bootstrap scan — same contract as
  `SclBootstrap_setProgressCallback` itself.

  **Deliberately absent:** `Orchestration_setReportCallback`/`_setGooseRecordCallback`. Those
  DATA-record slots (the actual `MmsReportRecord`/`GooseSubscriberRecord` payloads, as opposed to
  status/diagnostics) are always, unconditionally wired to `IpcDispatcher_onMmsReport`/
  `_onGooseRecord` by `Orchestration_run*` itself — `ipc_dispatcher`'s own ownership-transfer
  contract says exactly one consumer may ever own+destroy a given record, and `ipc_dispatcher` is
  that consumer. There is no way to register a second one.

- **`OrchestrationError Orchestration_run(OrchestrationHandle handle, LinkedList hostList, int mmsPort, const char* iedName, const char* interfaceId, AccessMode accessMode, OrchestrationErrorDetail* outDetail)`**
  — blocking. Full stage sequence: `IPC_DISPATCHER_START` → `BOOTSTRAP` → `STAGING` →
  `IED_NAME_RESOLUTION` (only if `iedName` is `NULL`/empty) → `MODEL_LOAD` →
  `REPORT_CLIENT_START` → `GOOSE_SUBSCRIBER_START`. Argument validation (before any stage runs):
  rejects `NULL` handle, an already-running handle, `NULL`/empty `hostList`, `mmsPort <= 0`,
  `NULL`/empty `interfaceId` — all with `ORCHESTRATION_ERR_INVALID_ARGUMENT`. **`iedName` empty is
  explicitly *not* a validation rejection** — it means "auto-detect from the staged SCL" (see §3).
  Returns once both long-running workers' own `_start()` calls have returned — this does **not**
  mean a report/GOOSE frame has actually arrived yet or the MMS association is fully up, only that
  synchronous setup succeeded. Not re-entrant: `ORCHESTRATION_ERR_INVALID_ARGUMENT` immediately if
  already running (call `Orchestration_stop()` first). `outDetail` is optional (NULL-safe), filled
  only on error, zeroed at entry either way.

- **`OrchestrationError Orchestration_runFromLocalFile(OrchestrationHandle handle, const char* sclFilePath, const char* host, int mmsPort, const char* iedName, const char* interfaceId, AccessMode accessMode, OrchestrationErrorDetail* outDetail)`**
  — same end state as `Orchestration_run`, but `BOOTSTRAP`/`STAGING` are never reached:
  `sclFilePath` is read directly (never modified or deleted — the caller's own file, not staged or
  owned by this handle). `host`/`mmsPort` are still required and still drive the real
  `mms_report_client`/`goose_subscriber` connections — loading the model locally only replaces
  *how the SCL description is obtained*, never the live MMS/GOOSE target. Empty `iedName` still
  triggers the same auto-detect-from-SCL behavior. Stage sequence:
  `IPC_DISPATCHER_START` → `IED_NAME_RESOLUTION` (if empty) → `MODEL_LOAD` →
  `REPORT_CLIENT_START` → `GOOSE_SUBSCRIBER_START`. Same fail-hard/re-runnable/re-entrancy
  guarantees as `Orchestration_run`.

- **`OrchestrationError Orchestration_runFromOnlineDiscovery(OrchestrationHandle handle, const char* host, int mmsPort, const char* iedName, const char* interfaceId, AccessMode accessMode, const char* acseAuthPassword, OrchestrationErrorDetail* outDetail)`**
  — a third way to obtain the model: instead of parsing SCL at all, walks the live device's own
  MMS ACSI directory/model-discovery services (`IedModelOnlineLoader_build`) to reconstruct an
  equivalent model directly. `host`/`mmsPort`/`interfaceId` are required — there's no bootstrap
  step to derive them from a scanned candidate here. **`iedName` behaves differently from the
  other two entry points**: it only *labels* the constructed model (there's no SCL `<IED>` list to
  auto-detect from over a live connection), so an empty `iedName` is *not* a resolution stage of
  its own — it's passed straight through, defaulting inside `ied_model_online_loader`.
  `acseAuthPassword` is applied unconditionally to the one connection this makes, no
  retry-on-rejection (this call always targets one already-known host — the same posture
  `mms_report_client` already takes). Stage sequence: `IPC_DISPATCHER_START` →
  `ONLINE_DISCOVERY` (replaces `BOOTSTRAP`/`STAGING`/`MODEL_LOAD` entirely) →
  `REPORT_CLIENT_START` → `GOOSE_SUBSCRIBER_START`. Same fail-hard/re-runnable/re-entrancy
  guarantees. **This is a deliberately explicit, caller-invoked fallback, never a silent branch
  inside `Orchestration_run` itself** — see §6.

- **Fail-hard/reverse-teardown guarantee (applies to all three entry points):** on any stage's
  failure, everything started by *earlier stages in that same call* is torn down in reverse order
  before returning — including stopping `ipc_dispatcher` if a later stage fails, since it's always
  started first. The handle is left in a clean, re-runnable state: a caller can retry the same or
  a different `Orchestration_run*` call on the same handle after a failure with no manual cleanup.
  `handle->running` is only ever set `true` at the very end of the shared tail
  (`runFromIedModelHandle`), after both workers' `_start()` calls succeed — so a failed run never
  leaves `running == true`.

- **`void Orchestration_stop(OrchestrationHandle handle)`** — stops `goose_subscriber`, then
  `mms_report_client`, then `ipc_dispatcher` (in that order — guarantees no more producer-thread
  calls can land on `ipc_dispatcher` once it's torn down), then releases `ied_model`. Blocking
  (every underlying `_stop()`/`_destroy()` call blocks). Must be called from the caller's own
  thread, never from within a registered callback (same deadlock rule as the wrapped features).
  Safe to call repeatedly, or on a never-run handle (no-op) — including tearing down
  `ipc_dispatcher` even if `Orchestration_run*` was never called or failed before reaching later
  stages, since `ipc_dispatcher` exists from `Orchestration_create` onward.

- **`void Orchestration_destroy(OrchestrationHandle handle)`** — implies `Orchestration_stop()` if
  still running, then frees the handle including `ipc_dispatcher` (which always exists once
  `Orchestration_create` succeeds).

## 3. Per-file breakdown

### `service/orchestration_api.h` / `orchestration_api.c`
The only public header, and the file with the actual sequencing logic (448 lines).

**`OrchestrationConfig_defaults`** — thin fan-out to each embedded sub-config's own `_defaults`.

**`Orchestration_create`** — `calloc`s `struct sOrchestrationHandle`, copies in the config (or
defaults), then calls `IpcDispatcher_create` (alloc-only). Frees the handle and returns NULL on
either allocation failure.

**Callback setters** — each just stores the callback + userParam pair on the handle; nothing else.

**`runFromIedModelHandle` (private, static)** — the shared tail for *all three* entry points:
starts `mms_report_client`, then `goose_subscriber`, with fail-hard rollback between every step.
Assumes `ipc_dispatcher` is already started (every caller's own stage 0) and **takes ownership of
`iedModel`** — on any failure inside this function, `iedModel` is released and `ipc_dispatcher` is
rolled back before returning. Concretely:
1. `MmsReportClient_create(iedModel, host, port, &config.reportClientConfig, ...)` — `host`/`port`
   are whichever the caller resolved (the winning `scl_bootstrap` candidate's own host/port, the
   local-file path's caller-supplied host/port, or the online-discovery connection's own
   host/port). On failure: release `iedModel`, stop `ipc_dispatcher`, stage =
   `ORCHESTRATION_STAGE_REPORT_CLIENT_START`.
2. `MmsReportClient_setReportCallback(reportClient, IpcDispatcher_onMmsReport, ipcDispatcher)` —
   unconditional, always wired regardless of caller config. The optional
   `connStateCallback`/`rcbStatusCallback`, if registered via the setters in §2, are wired here
   too.
3. `MmsReportClient_start(reportClient)`. On failure: destroy `reportClient`, release `iedModel`,
   stop `ipc_dispatcher`, same stage tag.
4. `GooseSubscription_create(iedModel, interfaceId, &config.gooseSubscriberConfig, ...)`. On
   failure: destroy the already-started `reportClient` too, release `iedModel`, stop
   `ipc_dispatcher`, stage = `ORCHESTRATION_STAGE_GOOSE_SUBSCRIBER_START`.
5. `GooseSubscription_setRecordCallback(gooseHandle, IpcDispatcher_onGooseRecord, ipcDispatcher)`
   — unconditional, same pattern as step 2. Optional `gooseStatusCallback` wired here too.
6. `GooseSubscription_start(gooseHandle)`. On failure: same full rollback as step 4.
7. On success: `handle->iedModel`/`reportClient`/`gooseSubscriber` are stored and
   `handle->running = true`.

**`runFromSclFile` (private, static)** — the shared continuation for `Orchestration_run` (network-
fetched SCL) and `Orchestration_runFromLocalFile` (caller-supplied SCL): IED-name auto-detection,
then `ied_model` load, then `runFromIedModelHandle`. Identical regardless of where `sclPath`'s
bytes came from. Assumes `ipc_dispatcher` is already started and rolls it back on any failure here.
Takes a `sclPathIsOwnedTempFile` flag: `true` for the `scl_bootstrap` path (`sclPath` is a
heap-allocated `mkstemp` path this function must `unlink`+`free` once `IedModel_loadFromFile` has
parsed it into memory — it has zero purpose afterward); `false` for the local-file path (`sclPath`
is the caller's own file, never touched beyond reading it).

  - **IED-name auto-detection** — only entered when `iedName` is `NULL`/empty:
    `IedModel_listIedNames(sclPath, &listErr)` lists every `<IED name="...">` in the staged/local
    SCL. `discoveredCount = iedNames ? LinkedList_size(iedNames) : 0`. If the list call failed
    (`!iedNames`) *or* `discoveredCount != 1`, this is a hard failure —
    `ORCHESTRATION_ERR_IED_NAME_RESOLUTION_FAILED`, stage = `ORCHESTRATION_STAGE_IED_NAME_RESOLUTION`,
    `outDetail->discoveredIedCount` and `outDetail->iedNameListError` set (the latter only
    meaningful if listing itself errored — `IED_MODEL_OK` if it succeeded but returned 0 or >1
    names). **No interactive retry** for the ambiguous case — an accepted limitation (see §5).
    Exactly one name: `strdup`'d into `autoDetectedIedName`, used as `resolvedIedName`.
  - **`ied_model` load** — `IedModel_loadFromFile(sclPath, resolvedIedName, accessMode, &modelErr)`
    parses the *entire* file into memory in one call, so an owned temp file has zero purpose
    afterward: `OrchestrationStaging_cleanup(sclPath)` + `free((char*)sclPath)` run immediately
    after this call **regardless of outcome** (not deferred). A caller-supplied local file is left
    untouched either way — not this function's file to delete. On load failure:
    `ORCHESTRATION_ERR_MODEL_LOAD_FAILED`, stage = `ORCHESTRATION_STAGE_MODEL_LOAD`.
  - On success: falls through to `runFromIedModelHandle(handle, iedModel, host, port, interfaceId,
    outDetail)`.

**`Orchestration_run`** — validates arguments (§2), then: stage 0 `IpcDispatcher_start` (no
rollback needed on failure — nothing else has started); stage 1 `scl_bootstrap`:
`SclBootstrap_create` (short-lived handle, *not* stored on `OrchestrationHandle` since
`scanAndFetch` is one-shot), optional progress-callback wiring, `SclBootstrap_scanAndFetch(handle,
hostList, mmsPort, ...)`. The result list is handed to
`OrchestrationUseCases_selectAndDetachFirstRetrieved` (see next section) to pick the first
`SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED` candidate; if none qualifies,
`OrchestrationUseCases_summarizeBootstrapFailure` captures the last candidate's status for
diagnostics (`outDetail->lastCandidateStatus`), the remaining results list is destroyed, and this
returns `ORCHESTRATION_ERR_BOOTSTRAP_FAILED` at stage `ORCHESTRATION_STAGE_BOOTSTRAP`. On a winner:
the rest of `results` is destroyed via `LinkedList_destroyDeep(results,
SclBootstrap_destroyResult)` — the winner already detached, so it survives. Stage 2 (staging):
`OrchestrationStaging_writeTempFile(winner->fileData, winner->fileSize, &stageErrno)`; failure →
`ORCHESTRATION_ERR_STAGING_FAILED`, stage = `ORCHESTRATION_STAGE_STAGING`. Stages 3–5: delegated to
`runFromSclFile(handle, tempPath, /*owned=*/true, winner->host, winner->port, iedName,
interfaceId, accessMode, outDetail)` — note **the host/port used for the live MMS/GOOSE connection
is the winning bootstrap candidate's own host/port**, not a separately supplied parameter:
bootstrap and reporting always target the same physical IED. `winner` is destroyed after
`runFromSclFile` returns either way.

**`Orchestration_runFromLocalFile`** — validates arguments (additionally requiring non-empty
`sclFilePath`/`host`, distinct from `Orchestration_run`'s `hostList`), stage 0
`IpcDispatcher_start`, then delegates straight to `runFromSclFile(handle, sclFilePath,
/*owned=*/false, host, mmsPort, iedName, interfaceId, accessMode, outDetail)` — no
`scl_bootstrap`/staging stage exists in this path at all.

**`Orchestration_runFromOnlineDiscovery`** — validates arguments, stage 0 `IpcDispatcher_start`,
then `IedModelOnlineLoader_build(host, mmsPort, iedName, accessMode, acseAuthPassword, NULL,
&loaderErr)` — this call owns its own one-shot `IedConnection` end to end (connect, discover,
disconnect); `orchestration` never touches `IedConnection` directly here, matching
`ied_model_online_loader`'s own documented "zero direct third-party includes" invariant for its
callers. Failure → `ORCHESTRATION_ERR_ONLINE_DISCOVERY_FAILED`, stage =
`ORCHESTRATION_STAGE_ONLINE_DISCOVERY`, `ipc_dispatcher` rolled back. Success → straight into
`runFromIedModelHandle(handle, iedModel, host, mmsPort, interfaceId, outDetail)` — no IED-name
resolution stage exists on this path (see §2's note on `iedName` behaving differently here).

**`Orchestration_stop`** — destroys `gooseSubscriber` (if non-NULL), then `reportClient`, then
`IpcDispatcher_stop` (always called, even if never started — no-op in that case), then releases
`iedModel`. Sets `handle->running = false` unconditionally at the end.

**`Orchestration_destroy`** — `Orchestration_stop` + `IpcDispatcher_destroy` (always exists once
`Orchestration_create` succeeded) + `free(handle)`.

### `domain/orchestration_types.h`
Domain vocabulary — pulls in every sibling feature's public `_api.h` (never their `domain`/`data`
headers directly, same rule every feature states about itself). Unlike `ied_model`/
`mms_report_client`/`goose_subscriber`, **this layer has zero direct third-party includes of its
own** — only transitively, via the sibling `_api.h` headers — since its job is sequencing already-
implemented features, not talking to `libiec61850`/`libwebsockets`/`cJSON` itself.

Contents:
- **`OrchestrationError`** — `OK`, `ERR_INVALID_ARGUMENT`, `ERR_OUT_OF_MEMORY`,
  `ERR_IPC_DISPATCHER_FAILED`, `ERR_BOOTSTRAP_FAILED`, `ERR_STAGING_FAILED`,
  `ERR_IED_NAME_RESOLUTION_FAILED`, `ERR_MODEL_LOAD_FAILED`, `ERR_REPORT_CLIENT_FAILED`,
  `ERR_GOOSE_SUBSCRIBER_FAILED`, `ERR_ONLINE_DISCOVERY_FAILED` (the last is
  `Orchestration_runFromOnlineDiscovery`-only: the live device never gave up a usable model at all
  — connect failure or zero logical devices).
- **`OrchestrationStage`** — `NONE` (success, or failure before any stage began — bad args),
  `IPC_DISPATCHER_START`, `BOOTSTRAP`, `STAGING`, `IED_NAME_RESOLUTION` (only entered when
  `iedName` empty), `MODEL_LOAD`, `ONLINE_DISCOVERY` (`_runFromOnlineDiscovery`-only, replaces
  `BOOTSTRAP`/`STAGING`/`MODEL_LOAD`), `REPORT_CLIENT_START`, `GOOSE_SUBSCRIBER_START`.
- **`OrchestrationErrorDetail`** — which stage failed plus that stage's own underlying error code
  (mirrors `SclBootstrapResult`'s own `status`+`lastMmsError` two-part pattern rather than
  inventing new error vocabulary): `ipcDispatcherError`, `bootstrapArgError` (scanAndFetch itself
  returned NULL — bad args/OOM), `lastCandidateStatus` (scanAndFetch succeeded but no candidate
  reached `FILE_RETRIEVED`), `stagingErrno`, `discoveredIedCount` (0 = no `<IED>` at all, >1 =
  ambiguous), `iedNameListError`, `modelLoadError`, `onlineDiscoveryError`, `reportClientError`,
  `gooseSubscriberError`. Only the field(s) matching `.stage` are meaningful; the rest are
  zero-valued.
- **`OrchestrationConfig`** — bundles `IpcDispatcherConfig`, `SclBootstrapConfig`,
  `MmsReportClientConfig`, `GooseSubscriberConfig`. `bootstrapConfig.acseAuthPassword` stays
  borrowed — same convention as `SclBootstrapConfig` itself, caller must keep it alive for the
  `OrchestrationHandle`'s whole lifetime.
- **`struct sOrchestrationHandle`** — defined here (not behind an extra internal header) because
  every file in this feature needs field access, mirroring every sibling feature's `struct
  sXHandle` convention: opacity is enforced by which header is exposed
  (`service/orchestration_api.h` is the only public one), not by hiding the struct. Fields:
  `config`; `ipcDispatcher` (owned for the handle's whole lifetime — created in
  `Orchestration_create`, started as stage 0 of every `Orchestration_run*`, stopped in
  `Orchestration_stop`, destroyed in `Orchestration_destroy`; unlike `iedModel`/`reportClient`/
  `gooseSubscriber`, never NULL after a successful `_create` — callers have no other way to reach
  `ipc_dispatcher`, by design); `iedModel`/`reportClient`/`gooseSubscriber` (owned once `_run*`
  succeeds, else NULL); the four diagnostic-callback + userParam pairs; `volatile bool running`.

### `domain/orchestration_usecases.c` / `.h`
Pure sequencing/selection logic over `SclBootstrap_scanAndFetch`'s result list — no I/O, no
`libiec61850` symbols beyond `LinkedList` itself, same posture as `scl_bootstrap`'s own
`domain/scl_bootstrap_usecases.c`.

- **`SclBootstrapResult* OrchestrationUseCases_selectAndDetachFirstRetrieved(LinkedList results)`**
  — scans `results` in input order for the first element whose `.status ==
  SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED`, detaches it via `LinkedList_remove` (does **not** free
  it) and returns it — so it survives a subsequent `LinkedList_destroyDeep(results,
  SclBootstrap_destroyResult)` of whatever remains. Returns NULL if no candidate reached
  `FILE_RETRIEVED` (or on NULL/empty `results`) — `results` is left fully intact in that case.
  Caller owns the returned result.
- **`SclBootstrapCandidateStatus OrchestrationUseCases_summarizeBootstrapFailure(LinkedList results)`**
  — returns the `.status` of the *last* element in `results`, a cheap best-effort diagnostic for
  `OrchestrationErrorDetail.lastCandidateStatus` when no candidate reached `FILE_RETRIEVED`.
  Returns `SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER` on NULL/empty `results` (defensive —
  `scanAndFetch` never actually returns an empty non-NULL list for a non-empty `hostList`, but
  this keeps the helper total).

### `data/orchestration_staging.c` / `.h`
Bridges `scl_bootstrap`'s in-memory SCL bytes (`SclBootstrapResult.fileData`/`fileSize`) to
`ied_model`'s file-path-only `IedModel_loadFromFile`. Deliberately decoupled from every other
orchestration type — no `LinkedList`/`IedModelHandle` dependency — so this stays trivially
unit-testable in isolation (and is: see §7, one of only two self-contained temp-file unit-test
cases in the whole repo, per `CLAUDE.md`'s Testing section).

- **`char* OrchestrationStaging_writeTempFile(const uint8_t* fileData, uint32_t fileSize, int* outErrno)`**
  — `strdup`s the template `"/tmp/orchestration_scl_XXXXXX"`, calls `mkstemp` on it, then writes
  `fileSize` bytes from `fileData` in a loop that retries on `EINTR`. Returns NULL + sets
  `*outErrno` on `NULL fileData` (`EINVAL`), `strdup` failure (`ENOMEM`), or any `mkstemp`/`write`/
  `close` failure (the real `errno`, with the partially-written file `unlink`ed + path freed before
  returning). On success, returns the owned, heap-allocated path (caller `free()`s it) with
  `*outErrno = 0`.
- **`void OrchestrationStaging_cleanup(const char* path)`** — best-effort `unlink`; logs a warning
  to `stderr` on failure but is never treated as fatal by callers (an orphaned file under `/tmp` is
  a nuisance, not a correctness issue). NULL-safe no-op.

### `utils/orchestration_utils.c` / `.h`
Logging-only string helpers — never used for control flow.

- **`char* OrchestrationUtils_safeStringDup(const char* s)`** — NULL-safe `strdup` (returns NULL
  for NULL input; otherwise a fresh independent heap copy the caller owns).
- **`const char* OrchestrationUtils_errorToString(OrchestrationError err)`** — human-readable
  description for every `OrchestrationError` value; a `default:` branch covers any unrecognized
  value. Always non-NULL.
- **`const char* OrchestrationUtils_stageToString(OrchestrationStage stage)`** — same pattern for
  every `OrchestrationStage` value.
- **`const char* OrchestrationUtils_candidateStatusToString(SclBootstrapCandidateStatus status)`**
  — explains *why* the `BOOTSTRAP` stage failed at the candidate level (the generic
  stage/error pair alone only says *which* stage failed and *that* `scl_bootstrap` failed, not the
  specific reason — e.g. `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND`: "associated fine, but no SCL
  file (.icd/.cid/.scd/.ssd/.sed) found in its file directory"). Extracted from `main.c`'s own
  original private helper of the same shape (pre-`device_manager` version) so
  `control_dispatcher`'s control-plane error-mapping and `main.c`'s own diagnostics share one copy
  instead of duplicating the string table.

## 4. Threading & concurrency model

- **`Orchestration_run`/`_runFromLocalFile`/`_runFromOnlineDiscovery` are each one blocking call**
  on the calling thread — there is no orchestration-owned background thread anywhere in this
  layer. The call returns only once every synchronous stage (including both workers' own
  `_start()` calls) has completed or one has failed.
- What each call **starts** (owned by the sub-features, not by `orchestration` itself):
  - `ipc_dispatcher`'s libwebsockets service-loop thread, started by `IpcDispatcher_start` at
    stage 0 of every entry point.
  - `mms_report_client`'s dedicated reconnect-supervisor thread, started inside
    `MmsReportClient_start` — driven by `IedConnection`'s state-changed handler, handles
    exponential-backoff reconnects for the whole lifetime of the report client.
  - `goose_subscriber`'s reception thread (library-managed, via `GooseReceiver_start()`) plus its
    own low-rate liveness-polling thread (watches `GooseSubscriber_isValid()` per target), both
    started inside `GooseSubscription_start`.
- **What stays synchronous, on the caller's own thread:** `scl_bootstrap`'s entire
  `SclBootstrap_scanAndFetch` call (phase-1 bounded-concurrency TCP probe + phase-2 sequential MMS
  sessions — see `scl_bootstrap`'s own doc for its internal concurrency model, which uses
  non-blocking I/O on the calling thread rather than OS threads), `OrchestrationStaging_writeTempFile`,
  `IedModel_listIedNames`/`IedModel_loadFromFile`, and `IedModelOnlineLoader_build`'s one-shot
  `IedConnection` walk (for the online-discovery path). All of these block the thread that called
  `Orchestration_run*` for their entire duration — a caller wanting non-blocking behavior (e.g.
  `device_manager` fielding a `START_REPORTING` control-plane request) is responsible for running
  the call on its own worker thread; `orchestration` makes no promise about that.
- **Report/GOOSE data-record delivery is cross-thread by construction**: `mms_report_client`'s
  supervisor thread and `goose_subscriber`'s reception thread both call directly into
  `IpcDispatcher_onMmsReport`/`_onGooseRecord` (unconditionally wired by `runFromIedModelHandle`),
  which — per `ipc_dispatcher`'s own threading contract — only serializes to JSON and pushes onto
  a bounded, mutex-guarded ring buffer, never touching `lws_write` directly from a producer
  thread.
- **No locks inside `orchestration` itself** — `handle->running` is a `volatile bool`, but nothing
  in this layer contends on it concurrently by design: `Orchestration_run*` is documented
  not-re-entrant, and `Orchestration_stop`/`_destroy` are documented as caller-thread-only, never
  callable from within a registered callback (the same deadlock rule the wrapped features
  themselves state).

## 5. Known limitations / deliberate scope boundaries

- **Single-IED scope.** One `OrchestrationHandle` sequences one physical IED's full pipeline.
  Running several concurrently (several devices) is explicitly `src/device_manager/`'s job — it
  supplies the missing registry (deviceId → `OrchestrationHandle`), port allocation, and
  three-phase locking on top of this layer; `orchestration` itself has no notion of "more than one
  IED" anywhere in its types.
- **No interactive retry on ambiguous IED-name auto-detection.** If the staged/local SCL declares
  zero or more than one `<IED>` and the caller left `iedName` empty, `ORCHESTRATION_STAGE_IED_NAME_RESOLUTION`
  fails hard immediately — there's no prompt-and-retry loop. The caller must supply an explicit
  `iedName` and re-run.
- **`Orchestration_runFromOnlineDiscovery` is never automatic.** It is exclusively an explicit,
  caller-invoked fallback — never a silent branch inside `Orchestration_run` based on how bootstrap
  happened to fail. Online discovery is materially slower/more request-heavy than one SCL transfer
  + local parse, and changing `Orchestration_run`'s own behavior based on the failure mode would
  violate this layer's fail-hard, no-silent-branching contract. `device_manager`'s bootstrap
  policy is the intended caller of the one-shot retry (see §6).
- **`Orchestration_runFromOnlineDiscovery`'s `iedName` doesn't participate in resolution** — unlike
  the other two entry points, there's no SCL `<IED>` list to auto-detect from over a live
  connection, so it's purely a label passed through to `ied_model_online_loader`.
- **`Orchestration_stop` cannot interrupt in-flight sub-feature work mid-call** — it blocks on each
  underlying `_stop()`/`_destroy()` in turn; how long that takes is entirely up to
  `mms_report_client`/`goose_subscriber`/`ipc_dispatcher`'s own teardown paths, not something
  `orchestration` can bound.
- **Zero direct third-party includes.** By design, this layer never touches `libiec61850`/
  `libwebsockets`/`cJSON` symbols directly — every capability comes through a sibling feature's
  public `_api.h`. A bug that looks like it needs an orchestration-level fix to protocol behavior
  almost always belongs in the wrapped feature instead.
- **Config wiring is all-or-nothing at `Orchestration_create` time.** There's no way to change
  `OrchestrationConfig` (e.g. `ipcDispatcherConfig.port`) after the handle exists — a new port
  needs a new handle.

## 6. Cross-feature dependencies

**Calls into (via each feature's own public `service/*_api.h` only):**
- `ipc_dispatcher` — lifecycle owned entirely by this layer: `IpcDispatcher_create` at
  `Orchestration_create`, `IpcDispatcher_start`/`_stop` bracketing every `Orchestration_run*`/
  `_stop`, `IpcDispatcher_destroy` at `Orchestration_destroy`. Its two callback slots
  (`IpcDispatcher_onMmsReport`/`_onGooseRecord`) are unconditionally registered onto
  `mms_report_client`/`goose_subscriber` by `runFromIedModelHandle` — the only consumer either
  worker's data records ever reach.
- `scl_bootstrap` — `Orchestration_run` only (stage 1): short-lived handle, created and destroyed
  within the single call, never stored on `OrchestrationHandle`.
- `ied_model` — `IedModel_listIedNames` (auto-detection) and `IedModel_loadFromFile` (parse),
  called from `runFromSclFile`, used by both `Orchestration_run` and `_runFromLocalFile`. The
  resulting `IedModelHandle` is stored on the `OrchestrationHandle` and released in
  `Orchestration_stop`.
- `ied_model_online_loader` — `IedModelOnlineLoader_build`, called **only** from
  `Orchestration_runFromOnlineDiscovery`, **never automatically inside `Orchestration_run`
  itself**. This is the one narrow, deliberate exception to the "No over-the-wire tree discovery"
  Hard Rule (`CLAUDE.md`) — only engaged by `device_manager`'s bootstrap policy, as a one-shot
  retry after `Orchestration_run` fails with exactly `ORCHESTRATION_ERR_BOOTSTRAP_FAILED` and
  `outDetail.lastCandidateStatus == SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` (a connectable IED —
  e.g. OMICRON IED Scout's "Simulate IED" mode — that never serves an SCL file).
- `mms_report_client` — `MmsReportClient_create`/`_setReportCallback`/`_setConnectionStateCallback`/
  `_setRcbStatusCallback`/`_start`, called from `runFromIedModelHandle` for all three entry points.
  Stored on the handle, destroyed in `Orchestration_stop`.
- `goose_subscriber` — `GooseSubscription_create`/`_setRecordCallback`/`_setStatusCallback`/
  `_start`, same shared tail. Stored on the handle, destroyed first in `Orchestration_stop`
  (before `mms_report_client`, so no producer can still be feeding `ipc_dispatcher` when it's torn
  down).

**Called by:**
- `main.c` — indirectly only, never directly today: per `CLAUDE.md`'s Current State, `main.c`
  wires `device_manager`, and `device_manager` is the one that calls `Orchestration_run*` per
  device.
- `src/device_manager/` — its bootstrap policy holds the shared "start a device" sequencing
  function (local file vs. `scl_bootstrap`-with-online-discovery-retry), calling into exactly one
  of the three `Orchestration_run*` entry points per `START_REPORTING` request, and
  `Orchestration_stop`/`_destroy` per `STOP_REPORTING`.

**Hard Rules this layer must never violate** (from `CLAUDE.md`, repo-wide but concretely
constrains this layer):
- Never hand-roll GOOSE/MMS parsing — this layer never touches `libiec61850` directly at all; every
  protocol call goes through a sibling feature.
- **No over-the-wire tree discovery**, except the one narrow `ied_model_online_loader` exception
  above, and only via the explicit `Orchestration_runFromOnlineDiscovery` entry point.
- No cyclic polling — this layer starts nothing that polls except what `goose_subscriber`'s own
  liveness thread already does internally (that feature's own narrow exception, not
  `orchestration`'s).
- No dangling connections — `Orchestration_stop`'s ordering (GOOSE → report client → ipc_dispatcher
  → model release) exists specifically to guarantee no producer can leave a connection or a
  still-firing callback dangling against a torn-down `ipc_dispatcher`.

## 7. Tests

**`tests/orchestration/`** (Unity unit tests, wired into `tests/Makefile`'s explicit `TESTS` list
as `test_orchestration_staging`, `test_orchestration_usecases`, `test_orchestration_utils`,
`test_orchestration_api`):

- **`test_orchestration_staging.c`** — one of only **two self-contained temp-file unit-test cases
  in the entire repo** (the other is `tests/ied_model/`, per `CLAUDE.md`'s Testing section — the
  only file I/O permitted at the unit level). Three cases: `writeTempFile` writes the exact input
  bytes to a real on-disk file and `cleanup` actually removes it (`access(path, F_OK)` fails
  afterward); `writeTempFile` returns NULL + a nonzero `outErrno` for `NULL fileData`; `cleanup`
  doesn't crash on a NULL path.
- **`test_orchestration_usecases.c`** — pure-logic coverage of both use-case functions against
  hand-built `SclBootstrapResult`/`LinkedList` fixtures (with a local `freeResult` helper, avoiding
  pulling in all of `scl_bootstrap`'s own data layer): `selectAndDetachFirstRetrieved` returns NULL
  and leaves the list untouched when nothing is `FILE_RETRIEVED`; picks the first
  `FILE_RETRIEVED` element among several and detaches exactly that one (list size drops by one,
  the other elements — including a *later* `FILE_RETRIEVED` one — survive); returns NULL on
  empty/NULL input. `summarizeBootstrapFailure` returns the last element's status; returns
  `SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER` on empty/NULL input.
- **`test_orchestration_utils.c`** — `safeStringDup` NULL-passthrough and independent-copy
  semantics (mutating the original doesn't affect the copy); `errorToString`/`stageToString`/
  `candidateStatusToString` each asserted non-NULL for every enum value plus one out-of-range
  value, proving every `switch`'s `default:` branch is reachable and safe.
- **`test_orchestration_api.c`** — **argument-validation-only wiring tests**, explicitly documented
  as never calling any `Orchestration_run*` against a real reachable host and never opening a real
  connection/socket (mirrors `test_goose_subscriber_api`/`test_mms_report_client_api`'s own
  convention) — real end-to-end behavior is `integration_tests/orchestration/`'s job. Covers:
  `Orchestration_create`/`_destroy` with NULL and explicit config; `Orchestration_run` rejecting
  NULL handle, NULL/empty `hostList`, non-positive `mmsPort`, NULL/empty `interfaceId`, and
  re-entry on an already-running handle (`handle->running` set directly — the struct is visible by
  convention per `orchestration_types.h`); confirms empty/NULL `iedName` is **not** rejected at
  validation, instead reaching `ORCHESTRATION_STAGE_BOOTSTRAP` (since nothing listens on
  `127.0.0.1:102` in this hermetic test); the mirror set of the same validation cases for
  `Orchestration_runFromLocalFile` (additionally: a nonexistent `sclFilePath` with empty `iedName`
  reaches `ORCHESTRATION_STAGE_IED_NAME_RESOLUTION` with `IED_MODEL_ERR_FILE_NOT_FOUND` — proving
  the auto-detect path is actually reached, not skipped); `Orchestration_stop`/`_destroy` as a
  no-op on a never-run handle and NULL-safe on a NULL handle.

**`integration_tests/orchestration/`** (`e2e_test_orchestration.c`, real `ied_simulator`
"Reporter1" IED in-process over loopback, needs `sudo` — `CAP_NET_RAW` inherited transitively from
the GOOSE-subscriber step every entry point always starts):

- **`test_fullSequence_bootstrapModelReportAndGoose_endToEnd`** — proves the complete
  `Orchestration_run` sequence against a live simulator whose MMS file services are pointed at
  `fixtures/served_files/` (a local copy of `scl_bootstrap`'s own fixture): `ipc_dispatcher` bind,
  real `scl_bootstrap` fetch of `reporter1.cid` over MMS, staging, model load, `mms_report_client`
  enabling `brcbMain` (observed via the `onRcbStatus` diagnostic callback), `goose_subscriber`
  reaching `VALID` (via `onGooseStatus`). Connects a hand-rolled minimal RFC6455 websocket client
  directly to orchestration's own `ipc_dispatcher` port (report/GOOSE DATA records are no longer
  observable via any orchestration-level setter — those don't exist — only over the real
  websocket, matching the unconditional-wiring design in §2). After flipping the simulator's
  `GGIO1.Ind1.stVal` indication, asserts both a real `MMS_REPORT` JSON envelope (`source.rcbReference
  == "Reporter1LD1/LLN0.BR.brcbMain"`) and a real `GOOSE` JSON envelope (`source.goCbRef ==
  "Reporter1LD1/LLN0$GO$gcbInd"`) arrive.
- **`test_onlineDiscoveryFallback_afterNoSclFileFound_endToEnd`** — proves the online-discovery
  fallback end to end, exactly matching the intended caller-side policy described in §2/§6: first,
  `Orchestration_run` against a simulator whose file services point at an empty
  `fixtures/no_scl_files/` directory genuinely fails at `ORCHESTRATION_STAGE_BOOTSTRAP` with
  `detail.lastCandidateStatus == SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` — the exact real-world
  precondition `ied_model_online_loader` exists for. Then `Orchestration_runFromOnlineDiscovery`
  against the *same* host/port succeeds and delivers real report/GOOSE JSON over the same
  `ipc_dispatcher` websocket mechanism the first test already proved. (Only `urcbDyn`, parented
  under `GGIO1`, actually enables in this scenario — `brcbMain`/`brcbDup`/`rcbMulti01`, parented
  under `LLN0`, have no reportable FC=ST/MX attributes on their own LN once every RCB's
  `dataSetName` comes back NULL from a live server that assigns no dataset by default, so
  `mms_report_client`'s dynamic-dataset fallback can't synthesize anything for them — not a defect
  in online discovery itself.)

**`integration_tests/orchestration_local_file/`** (`e2e_test_orchestration_local_file.c`, real
`ied_simulator` "Reporter1" IED in-process, needs `sudo`, same `CAP_NET_RAW` reason):

- **`test_localFile_enablesRptEnabledMaxInstanceRcb_usingSuffixedReference`** — regression test for
  a real-world failure found running this daemon against a live ABB REC650 via IED Scout: the
  device's SCD declares `<ReportControl name="rcb_A" ...><RptEnabled max="5">`, but the live
  server only answers on the wire to `"rcb_A01".."rcb_A05"`, never the bare name (fixed in
  `ied_model`'s SCL loader, not in `orchestration` itself). Proves the fix end to end with no
  vendor device involved: the simulator's own `"rcbMulti"`/`<RptEnabled max="5">` RCB (served as
  wire object `rcbMulti01`) is loaded via `Orchestration_runFromLocalFile` against the local
  fixture `fixtures/reporter1_rcb_instance_mismatch.cid`, and successfully delivers a report under
  the suffixed reference — alongside `brcbMain` (a normal `max="1"` RCB), proving the fix doesn't
  regress the no-suffix case. Also confirms `Orchestration_runFromLocalFile`'s host/port wiring:
  the model is loaded from a local file, but the live MMS connection and the resulting report both
  target the simulator's real host/port, never anything read out of the loaded model itself.
