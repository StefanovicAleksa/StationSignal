# scl_bootstrap

## 1. Overview

`scl_bootstrap` is a one-shot, synchronous discovery/probe utility: given a caller-supplied list
of candidate hosts, it TCP-probes each for an MMS listener on a given port, and for every host
that answers, opens a real MMS/ACSE association, browses its file directory, and fetches one SCL
file (`.icd`/`.cid`/`.scd`/`.ssd`/`.sed`) over standard MMS file services. It exists because
`orchestration`/`device_manager` need a device's own SCL description before `ied_model` can load
anything — this feature is the thing that goes and gets that file (or reports why it couldn't).

It does **not** load the fetched bytes into `ied_model` and does **not** enable GOOSE/MMS
reporting. What happens to the retrieved bytes afterward (e.g. staging to a temp file and handing
it to `IedModel_loadFromFile`) is entirely a wiring-layer decision made by the caller
(`src/orchestration/`). It sits below `orchestration` and beside `ied_discovery` (which reuses its
phase-1 TCP-probe machinery) in the feature-first layout. Public boundary:
`src/features/scl_bootstrap/service/scl_bootstrap_api.h`.

## 2. Public API surface

All declared in `src/features/scl_bootstrap/service/scl_bootstrap_api.h`.

- **`void SclBootstrapConfig_defaults(SclBootstrapConfig* config)`** — fills a config with the
  documented defaults (see §3, `scl_bootstrap_types.h`). NULL-safe no-op if `config` is NULL. No
  I/O, no allocation.

- **`SclBootstrapHandle SclBootstrap_create(const SclBootstrapConfig* config, SclBootstrapError* outError)`**
  — allocates only, no I/O. `config == NULL` means "use `SclBootstrapConfig_defaults`". Deep-copies
  `acseAuthPassword` into an internally owned buffer (`ownedAuthPassword`) so the handle stays
  valid independent of whatever buffer the caller's config struct pointed at. Returns NULL and
  sets `SCL_BOOTSTRAP_ERR_OUT_OF_MEMORY` on allocation failure.

- **`void SclBootstrap_setProgressCallback(SclBootstrapHandle handle, SclBootstrapProgressCallback callback, void* userParam)`**
  — optional. Fires **synchronously on the calling thread**, once per candidate, immediately
  after that candidate's result is finalized inside `SclBootstrap_scanAndFetch`. There is no
  background thread in this feature at all, so this is a plain reentrant function call, not a
  cross-thread delivery (contrast with `mms_report_client`/`goose_subscriber`'s callbacks, which
  really do cross threads). Purely for incremental progress reporting on a large host list — the
  return value of `scanAndFetch` is always the complete, authoritative result set regardless of
  whether this is registered.

- **`LinkedList SclBootstrap_scanAndFetch(SclBootstrapHandle handle, LinkedList hostList, int mmsPort, SclBootstrapError* outError)`**
  — the one blocking entry point. `hostList` is a `LinkedList` of `char*` candidate
  hostnames/IP literals, borrowed (not retained past the call, copied internally as needed).
  `mmsPort` is usually 102 but caller-supplied so non-standard ports/simulators work. Runs phase 1
  (bounded-parallel TCP probe of every candidate) then phase 2 (sequential, full MMS
  association + browse + fetch for every candidate that answered phase 1). Returns a `LinkedList`
  of `SclBootstrapResult*`, one per input candidate, **in input order** — including the ones that
  didn't pan out, so callers get a complete picture. Caller owns the list and its elements:
  `LinkedList_destroyDeep(list, SclBootstrap_destroyResult)`. Returns NULL and sets `*outError`
  **only** for argument-level failures (NULL/empty `hostList`, `mmsPort <= 0`, allocation
  failure) — per-candidate connect/fetch outcomes are reported via each result's `.status` field,
  never via `outError`.

- **`void SclBootstrap_destroyResult(void* result)`** — `LinkedListValueDeleteFunction`-compatible;
  frees one `SclBootstrapResult` including its owned `host`/`fileName`/`fileData` buffers.
  NULL-safe.

- **`void SclBootstrap_destroy(SclBootstrapHandle handle)`** — frees the handle (and
  `ownedAuthPassword`). No `_stop()` exists: `scanAndFetch` is synchronous, so there's never a
  background operation in flight — destroy is only ever called after `scanAndFetch` has already
  returned.

- **`bool* SclBootstrap_tcpProbeOnly(SclBootstrapHandle handle, LinkedList hostList, int mmsPort)`**
  — phase-1-only TCP reachability probe, exposed specifically for sibling features (`ied_discovery`)
  that need the same bounded-concurrency async TCP scan without the full MMS-association +
  browse/fetch. Thin wrapper reusing the exact `SclBootstrapTcpProbe_scan` machinery
  `scanAndFetch` uses internally for its own phase 1 — reused rather than duplicated because that
  async-connect sliding-window state machine is substantial and easy to get subtly wrong a second
  time (contrast with the small ACSE-auth-setup snippet `mms_report_client` duplicates from this
  feature instead of sharing). Reuses `handle->config.tcpProbeTimeoutMs`/`maxConcurrentTcpProbes`
  — no separate config parameter. Returns a newly malloc'd `bool[LinkedList_size(hostList)]` in
  the same order/ownership contract as `SclBootstrapTcpProbe_scan` (caller must `free()`). NULL on
  NULL handle/hostList, empty hostList, bad port, or allocation failure.

Threading contract for all of the above: nothing here spawns a thread of its own.
`SclBootstrap_scanAndFetch` and `SclBootstrap_tcpProbeOnly` block the calling thread for the
entire scan; the caller decides whether to run that on a worker thread.

## 3. Per-file breakdown

### `service/scl_bootstrap_api.h` / `scl_bootstrap_api.c`
The only public header. `scl_bootstrap_api.c` is pure orchestration glue, no protocol logic of its
own:
- `SclBootstrapConfig_defaults` sets `tcpProbeTimeoutMs=500`, `maxConcurrentTcpProbes=16`,
  `mmsConnectTimeoutMs=3000`, `mmsRequestTimeoutMs=0` (library default), `maxBrowseDepth=2`,
  `acseAuthPassword=NULL`.
- `SclBootstrap_create` `calloc`s the handle, copies in the config (or defaults), then
  `SclBootstrapUtils_safeStringDup`s the password into `ownedAuthPassword` and repoints
  `config.acseAuthPassword` at that owned copy.
- `SclBootstrap_scanAndFetch` validates arguments via `SclBootstrapUseCases_isHostListValid` and
  `mmsPort > 0`, calls `SclBootstrapTcpProbe_scan` once for the whole host list, then walks the
  host list a second time building one `SclBootstrapResult` per host: unreachable hosts
  (`reachable[index] == false`) are stamped `SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER` directly with
  no phase-2 attempt; reachable ones get `SclBootstrapMmsSession_run` called on them. The progress
  callback (if registered) fires right after each result is appended to the list. Note: if
  `calloc` for one `SclBootstrapResult` fails mid-loop, that candidate is silently skipped (no
  entry added for it, no error surfaced) — the loop continues to the next host.
- `SclBootstrap_destroyResult`/`SclBootstrap_destroy` are straightforward frees, both NULL-safe.
- `SclBootstrap_tcpProbeOnly` re-validates the same arguments as `scanAndFetch` and delegates
  straight to `SclBootstrapTcpProbe_scan`.

### `domain/scl_bootstrap_types.h`
Pure data definitions, no behavior. Key points:
- Domain vocabulary for this feature **is** libiec61850's `IedClientError` — same convention as
  `mms_report_client`'s domain layer, because this data genuinely is the feature's domain, not
  swappable infrastructure.
- `SclBootstrapError` — `OK` / `ERR_INVALID_ARGUMENT` / `ERR_OUT_OF_MEMORY`. Argument-level only.
- `SclBootstrapCandidateStatus` (per-candidate outcome, six values):
  - `SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER` — phase-1 TCP probe never connected or was refused.
  - `SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED` — TCP reachable but the MMS association
    (COTP/session/presentation/ACSE/MMS-initiate) was rejected/timed out, or a later
    browse/download step failed for a reason other than access control.
  - `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` — associated fine, browsed the tree, nothing
    matched the SCL extension allowlist within `maxBrowseDepth`. (This is the exact status
    `orchestration`'s bootstrap policy checks for to trigger the one-shot
    `Orchestration_runFromOnlineDiscovery` retry — see CLAUDE.md's "No over-the-wire tree
    discovery" Hard Rule.)
  - `SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED` — covers two distinct rejection points folded into one
    status: the connect itself refused at the ACSE level (empirically observed as
    `IED_ERROR_CONNECTION_REJECTED` when a server-side `AcseAuthenticator` rejects the
    association — not documented in the vendored headers), or a later browse/`GetFile` call
    rejected with `IED_ERROR_ACCESS_DENIED` at the MMS/file-service level. `lastMmsError` carries
    whichever was actually observed.
  - `SCL_BOOTSTRAP_CANDIDATE_DOWNLOAD_FAILED` — file located and access authorized, but `GetFile`
    itself errored or was interrupted mid-transfer.
  - `SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED` — success; `fileName`/`fileData`/`fileSize` populated.
- `SclBootstrapResult` — one candidate's full outcome: owned `host` copy, `port`, `status`,
  `lastMmsError` (diagnostics only, `IED_ERROR_OK` if not applicable), `authWasAttempted` (true
  if a password-auth retry connection was made), and on success only: owned `fileName` (server-side
  path as returned by directory browse), owned `fileData` (raw bytes exactly as received),
  `fileSize`.
- `SclBootstrapConfig` — `tcpProbeTimeoutMs`, `maxConcurrentTcpProbes`, `mmsConnectTimeoutMs`,
  `mmsRequestTimeoutMs` (0 = library default), `maxBrowseDepth` (2 = root + one subdirectory
  level), `acseAuthPassword` (NULL = never attempt ACSE_AUTH_PASSWORD retry; copied internally).
- `struct sSclBootstrapHandle` is defined here, not hidden behind a further internal header —
  every file in the feature needs field access, mirroring `ied_model`/`mms_report_client`'s
  `struct s*Handle` convention. Opacity is enforced by which header is exposed
  (`service/scl_bootstrap_api.h` only), not by hiding the struct.

### `domain/scl_bootstrap_usecases.h` / `scl_bootstrap_usecases.c`
Pure logic, no `IedConnection`/`Socket` awareness — takes plain strings/lists so it stays
unit-testable in isolation.
- `SCL_EXTENSIONS_BY_PRIORITY[]` — `{".cid", ".icd", ".scd", ".ssd", ".sed"}`, in that priority
  order (`.cid`, most specific — an instantiated/configured description of *this* live device, is
  preferred).
- `endsWithIgnoreCase` (static) — case-insensitive suffix match, the primitive under extension
  matching.
- `SclBootstrapUseCases_isDirectoryEntry(fileName)` — true if the name ends in `/`. Per its own
  doc comment: empirically confirmed against the vendored reference `IedServer`
  filestore-backed implementation that it never actually emits trailing-slash directory markers —
  `IedConnection_getFileDirectory(NULL)` against it returns one flat, fully recursive listing with
  each entry already carrying its full relative path (e.g. `"subdir/nested.icd"`). This function
  (and the recursive descent it enables in `scl_bootstrap_mms_session.c`) is kept as a defensive
  fallback for a server that follows a stricter reading of the IEC 61850-8-1 file directory
  service and does emit real subdirectory markers, but that path is **unexercised** against this
  repo's reference simulator.
- `SclBootstrapUseCases_isSclExtension(fileName)` — true iff `extensionPriority >= 0`.
- `SclBootstrapUseCases_extensionPriority(fileName)` — returns -1 for a directory entry or NULL,
  else the index into `SCL_EXTENSIONS_BY_PRIORITY` (0 = `.cid`, best) or -1 if no extension
  matches.
- `SclBootstrapUseCases_pickBestSclFile(sclFileCandidates)` — picks one file from a `LinkedList` of
  `char*` candidates (assumed already SCL-extension-filtered) by extension priority, breaking ties
  lexicographically (`strcmp`). Returns a **borrowed** pointer into the list's own element — caller
  must copy it before the list is destroyed. NULL for an empty/NULL list.
- `SclBootstrapUseCases_isHostListValid(hostList)` — non-NULL, non-empty, and no NULL/empty
  elements.

### `data/scl_bootstrap_auth.h` / `scl_bootstrap_auth.c`
Isolates ACSE authentication wiring behind one function.
- `SclBootstrapAuth_configurePasswordAuth(IedConnection conn, const char* password)` — NULL-safe
  no-op if either arg is NULL. Pulls the `MmsConnection` and its `IsoConnectionParameters` out of
  the not-yet-connected `IedConnection`, creates an `AcseAuthenticationParameter`, sets its
  mechanism to `ACSE_AUTH_PASSWORD` and the password, and installs it via
  `IsoConnectionParameters_setAcseAuthenticationParameter`. Must be called before
  `IedConnection_connect()` — auth is negotiated at association time.
- Documented open question in the header: whether `IsoConnectionParameters_setAcseAuthenticationParameter`
  takes ownership of the `AcseAuthenticationParameter` (freed automatically when the owning
  `IedConnection` is destroyed) is **not documented** at the setter in `iso_connection_parameters.h`.
  This function deliberately does not free it itself — if it leaks, the leak is bounded by the
  number of auth-retry attempts made during one scan (at most one extra `IedConnection` per
  candidate), not by long-running traffic. Comment flags revisiting with valgrind if this becomes
  a real concern.
- `password` must outlive the subsequent `IedConnection_connect()` call — the caller
  (`scl_bootstrap_mms_session.c`) always passes the handle's `ownedAuthPassword`, stable for the
  handle's whole lifetime.

### `data/scl_bootstrap_file_download.h` / `scl_bootstrap_file_download.c`
Growable in-memory accumulator for `IedConnection_getFile`'s `IedClientGetFileHandler` callback —
the vendored header's own doc comment requires the handler to copy each chunk elsewhere before
returning, and this is that elsewhere.
- `SclBootstrapFileDownloadBuffer { uint8_t* data; uint32_t size; uint32_t capacity; }`.
- `SclBootstrapFileDownloadBuffer_init` — zeroes the struct (NULL-safe).
- `SclBootstrapFileDownloadBuffer_free` — frees `data`, resets fields (NULL-safe); does **not**
  free the struct itself (stack-allocated by the caller).
- `SclBootstrapFileDownload_handler(void* parameter, uint8_t* chunk, uint32_t bytesRead)` —
  `IedClientGetFileHandler`-compatible. `bytesRead == 0` short-circuits to `true` (nothing to
  append). Otherwise grows the buffer via `realloc` to `size + bytesRead` (only when needed —
  `newSize > capacity`), `memcpy`s the chunk in, advances `size`. Returns `false` (stop transfer)
  only on `realloc` failure.

### `data/scl_bootstrap_mms_session.h` / `scl_bootstrap_mms_session.c`
Phase 2: the full per-candidate MMS lifecycle — connect, recursively browse the file directory
tree, pick one SCL file, download it. Deliberately **not unit-tested** — a live `IedConnection`
can't be meaningfully faked hermetically, matching `ied_model`'s `scl_loader`/`mms_report_client`'s
`connection.c` convention of proving this class of code E2E instead. Never leaves an
`IedConnection` open on return, regardless of outcome.
- `browseDirectoryRecursive(conn, directoryName, depthRemaining, err)` (static) — calls
  `IedConnection_getFileDirectory(conn, err, directoryName)` (NULL `directoryName` = root). On
  error, destroys whatever entries came back (still `LinkedList_destroyDeep`'d even on failure)
  and returns an empty match list — a failure recursing into one subdirectory does **not** abort
  browsing of its siblings (the caller loop continues regardless of `subErr`, which is discarded
  after being used only to decide the return value's contents implicitly, i.e. it doesn't even
  gate anything further). For each entry: if it's a directory (`isDirectoryEntry`) and
  `depthRemaining > 1`, recurses into it via `SclBootstrapUtils_joinPath` for the child path and
  merges its matches in, then frees the constructed sub-path and does a shallow
  `LinkedList_destroyStatic` of the recursive call's result list (elements were already
  transplanted into `matches`, not double-freed). If it's a plain file matching the SCL
  extension allowlist, its joined path is added directly. `depthRemaining=1` means "this directory
  only, don't recurse" — with the default `maxBrowseDepth=2` this means root + one subdirectory
  level.
- `runSequenceOnce(result, config, password)` (static) — runs connect → browse → pick → download
  exactly once on one fresh `IedConnection`, optionally with password auth configured first via
  `SclBootstrapAuth_configurePasswordAuth`. Sets connect/request timeouts from config only if
  non-zero (0 defers to library default). Returns `true` iff the specific failure at *any* stage
  was access-control-shaped (signals to the caller an auth retry might help):
  - Connect failure: `err == IED_ERROR_ACCESS_DENIED || err == IED_ERROR_CONNECTION_REJECTED`
    both count as "might be worth retrying with auth" (the empirically-confirmed
    `CONNECTION_REJECTED`-at-ACSE-level behavior documented above). Sets
    `SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED` only if this is true; the default status
    (`MMS_CONNECT_FAILED`, pre-set at function entry) stands otherwise. Destroys the connection
    and returns immediately — no browse attempted.
  - Browse failure: only `IED_ERROR_ACCESS_DENIED` counts as access-denied here (not
    `CONNECTION_REJECTED`, which is a connect-time-only code). Non-access-denied browse failure
    is stamped `MMS_CONNECT_FAILED` (reused, not a distinct status) rather than a browse-specific
    status. Closes and destroys the connection either way.
  - No SCL file chosen (`pickBestSclFile` returns NULL): `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND`,
    not access-denied, connection torn down.
  - Download failure: only `IED_ERROR_ACCESS_DENIED` counts as access-denied;
    otherwise `SCL_BOOTSTRAP_CANDIDATE_DOWNLOAD_FAILED`. Connection torn down either way.
  - Success: `SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED`, `fileName`/`fileData`/`fileSize` populated
    from the downloaded buffer, connection torn down.
- `SclBootstrapMmsSession_run(result, config, ownedAuthPassword)` — the public entry point. Sets
  `result->authWasAttempted = false`, runs `runSequenceOnce` once with `password=NULL`. If that
  attempt signaled access-denied **and** `ownedAuthPassword` is non-NULL, sets
  `authWasAttempted = true` and runs `runSequenceOnce` a second time on a **fresh** connection with
  the password configured — exactly one retry, no further attempts regardless of the second
  outcome.

### `data/scl_bootstrap_tcp_probe.h` / `scl_bootstrap_tcp_probe.c`
Phase 1: a cheap, bounded-timeout TCP liveness probe across the whole candidate list, built
directly on `hal_socket.h`'s async-connect primitives
(`Socket_connectAsync`/`Socket_checkAsyncConnectState`) — a sliding window of up to
`maxConcurrent` probes in flight at once, driven from **one control thread** (no OS thread pool:
each probe is just a bounded connect wait, not sustained I/O). A successful TCP connect here does
**not** mean "this is an MMS device" — only that something is listening on the port; that
stronger claim is reserved for phase 2's real `IedConnection_connect` COTP/session/presentation/
ACSE/MMS handshake.
- `ProbeSlot { Socket socket; int hostIndex; uint64_t startTimeMs; }` — `hostIndex = -1` marks an
  empty slot.
- `POLL_INTERVAL_MS = 10` — polling granularity for in-flight async connects, chosen short enough
  to stay responsive to per-probe timeouts without busy-spinning.
- `hostAt(hostList, index)` (static) — linear walk to the Nth element; called repeatedly (not
  cached into an array up front), so overall probe scheduling is O(total × maxConcurrent) in list
  walks — acceptable given `total` is a subnet-scan-sized host count, not unbounded.
- `SclBootstrapTcpProbe_scan(hostList, port, timeoutMs, maxConcurrent)` — validates
  `hostList`/`total > 0`; clamps `maxConcurrent` to `[1, total]`. Main loop, while
  `remaining > 0`:
  1. **Fill phase**: for each empty slot (`hostIndex == -1`), pull the next unstarted host,
     `TcpSocket_create()` + `Socket_connectAsync`. If either fails (out of resources / immediate
     refusal), that candidate is stamped unreachable **immediately**, without ever occupying a
     slot or waiting out `timeoutMs` — an immediate-refusal fast path.
  2. **Poll phase**: for each occupied slot, checks `Socket_checkAsyncConnectState`. `CONNECTED`
     → reachable, slot freed. `FAILED` or `elapsed >= timeoutMs` → unreachable, slot freed. Both
     destroy the socket.
  3. If nothing resolved this round and work remains, `Thread_sleep(POLL_INTERVAL_MS)`.
  Returns the `bool[total]` array (caller-owned, `free()`) or NULL on NULL/empty `hostList` or
  allocation failure of either the result array or the slot array.

### `utils/scl_bootstrap_utils.h` / `scl_bootstrap_utils.c`
Small reusable string helpers, shared by the data layer and the service layer — no
`IedConnection`/`Socket` awareness.
- `SclBootstrapUtils_safeStringDup(s)` — NULL-safe `strdup` (NULL in → NULL out); caller owns the
  result.
- `SclBootstrapUtils_joinPath(parentDir, entryName)` — the next-argument builder for
  `IedConnection_getFileDirectory`/`getFile`. NULL `entryName` → NULL. NULL/empty `parentDir` →
  copy of `entryName`. Otherwise: `parentDir` immediately followed by `entryName`, **no separator
  inserted** — subdirectory entry names already carry their own trailing `/` per the ACSI
  file-directory convention (see `isDirectoryEntry`'s doc comment above). Caller owns the result.

## 4. Threading & concurrency model

- No background thread anywhere in this feature. `SclBootstrap_scanAndFetch` and
  `SclBootstrap_tcpProbeOnly` are fully synchronous and block the calling thread for the entire
  operation — scanning a network is a "do it once, get a complete answer" operation, unlike
  `mms_report_client`/`goose_subscriber`'s long-running background workers.
- Phase 1 (`SclBootstrapTcpProbe_scan`) achieves its *concurrency* (up to `maxConcurrentTcpProbes`
  probes in flight) entirely through non-blocking async-connect + polling on the single calling
  thread — not OS threads. This is the async-connect state machine `ied_discovery` reuses via
  `SclBootstrap_tcpProbeOnly` rather than re-implementing.
- Phase 2 (`SclBootstrapMmsSession_run` over reachable candidates) is strictly **sequential**, one
  candidate at a time, one `IedConnection` fully connected/closed/destroyed per attempt (up to two
  attempts per candidate, for the auth retry) before moving to the next candidate.
- The progress callback, if registered, fires synchronously on the calling thread, inline within
  `scanAndFetch`'s loop — no dispatch, no queue, no cross-thread hazard.
- No locks anywhere in this feature: the handle is not shared/mutated concurrently by design
  (single blocking call at a time), and there's no shared mutable state between phase-1 probing
  and phase-2 sessions beyond the plain `bool[]` reachability array passed by value/pointer down
  the call stack.
- Whatever thread the caller runs `scanAndFetch`/`tcpProbeOnly` on is blocked for the whole
  duration — callers needing non-blocking behavior (e.g. `scan_orchestration`'s continuous
  background sweep) are responsible for putting this call on their own worker thread.

## 5. Known limitations / deliberate scope boundaries

- **No `_start`/`_stop` pair** — `SclBootstrap_scanAndFetch` *is* the one blocking entry point;
  there's never a background operation in flight for a `_stop()` to tear down.
- **Does not load into `ied_model` and does not enable reporting** — purely discovery/fetch. The
  caller decides what to do with the raw bytes.
- **`isDirectoryEntry`'s trailing-slash subdirectory-marker path is unexercised** against this
  repo's reference simulator — the real vendored `IedServer` filestore implementation returns one
  flat, fully recursive listing instead, so the recursive-descent branch in
  `browseDirectoryRecursive` is a defensive fallback for stricter-spec servers, not something the
  E2E suite proves works.
- **`scl_bootstrap_mms_session.c` is deliberately not unit-tested** — a live `IedConnection` can't
  be meaningfully faked hermetically; proven E2E instead (see §7).
- **`SclBootstrapAuth_configurePasswordAuth`'s ownership of `AcseAuthenticationParameter` is an
  open, undocumented question** in the vendored library — not freed by this code; bounded leak
  (at most one extra connection per candidate per scan), flagged for a future valgrind check
  rather than guessed at.
- **A `calloc` failure for one candidate's `SclBootstrapResult` inside `scanAndFetch`'s loop
  silently drops that candidate** from the result list — no error surfaced, no placeholder entry.
- **Extension allowlist and priority order are fixed** (`.cid` > `.icd` > `.scd` > `.ssd` > `.sed`),
  not configurable.
- **`maxBrowseDepth` default of 2** covers root + one subdirectory level only — deeper SCL file
  placement on a device wouldn't be found without raising the config value.
- **ACSE password auth is the only auth mechanism supported** (`ACSE_AUTH_PASSWORD`) — no
  certificate-based or other ACSE auth mechanisms.

## 6. Cross-feature dependencies

**Calls into:**
- libiec61850's `IedConnection`/`iec61850_client.h` API directly (connect, `getFileDirectory`,
  `getFile`) — no wrapping feature between this and the library, same as `mms_report_client`.
- `hal_socket.h`/`hal_time.h`/`hal_thread.h` (HAL layer) for the phase-1 async TCP probe.
- Nothing from `ied_model`, `mms_report_client`, or `goose_subscriber` — deliberately zero
  `IedModelHandle` dependency anywhere in this feature; it doesn't parse SCL, doesn't know about
  RCBs/GoCBs.

**Called by:**
- `src/orchestration/` — stage 1 of `Orchestration_run` (probe hosts, fetch SCL bytes), before
  staging to a temp file and handing off to `ied_model`. Also the thing whose
  `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` result triggers `device_manager`'s bootstrap policy
  to retry via `Orchestration_runFromOnlineDiscovery` (the one narrow
  `ied_model_online_loader` exception to the "No over-the-wire tree discovery" Hard Rule).
- `ied_discovery/` — reuses `SclBootstrap_tcpProbeOnly` directly for its own cheap bounded-
  concurrency TCP-probe stage, rather than reimplementing the async-connect sliding-window state
  machine. `ied_discovery` does **not** call `SclBootstrap_scanAndFetch` — its own second-stage
  verification is a real MMS/ACSE association immediately closed, no file browsing/SCL fetch
  (that's deferred to a later, separate `scl_bootstrap` run if the device is actually onboarded).

**Hard Rules this feature must never violate** (from CLAUDE.md, applies repo-wide but concretely
constrains this feature):
- Never hand-roll GOOSE or MMS parsing — all protocol handling goes through libiec61850
  (`IedConnection`), which this feature already does exclusively.
- Never load into `ied_model` or enable reporting — strictly out of scope per its own domain-types
  doc comment.
- Never touch `third_party/` — pre-built/vendored; if a header seems to be missing something (e.g.
  the `AcseAuthenticationParameter` ownership question), say so, don't hand-edit.

## 7. Tests

**`tests/scl_bootstrap/`** (Unity unit tests, hermetic, no I/O — wired into `tests/Makefile`'s
explicit `TESTS` list as `test_scl_bootstrap_usecases`/`test_scl_bootstrap_utils`):
- `test_scl_bootstrap_usecases.c` — covers the domain layer exhaustively:
  `isDirectoryEntry` (trailing-slash true/plain-file false/NULL-or-empty false),
  `isSclExtension` (all five recognized extensions, case-insensitivity, unrecognized/no-extension/
  NULL false, directory-entry-named-like-a-file false), `extensionPriority` (full cid<icd<scd<ssd<sed
  ordering, -1 for no match), `pickBestSclFile` (NULL for NULL/empty list, prefers `.cid` over
  `.icd`, breaks ties lexicographically), `isHostListValid` (false for NULL/empty, false when any
  element is NULL/empty, true for a valid list).
- `test_scl_bootstrap_utils.c` — covers the string helpers: `safeStringDup` (NULL passthrough,
  produces an independent copy that survives mutation of the original buffer),
  `joinPath` (returns a copy of `entryName` when parent is NULL/empty, concatenates without
  inserting a separator, handles nested-directory paths, NULL when `entryName` is NULL).

Both unit-test files together prove every pure-logic function in `domain/`/`utils/` — the two
files this codebase considers hermetically fakeable. `scl_bootstrap_mms_session.c` and
`scl_bootstrap_tcp_probe.c` (real `IedConnection`/`Socket` usage) have no unit tests, by design
(see §5) — proven E2E instead.

**`integration_tests/scl_bootstrap/`** (`e2e_test_scl_bootstrap.c`, real `ied_simulator`
"Reporter1" IED in-process, real loopback MMS association and file services via
`SimServer_setFilestoreBasepath` pointed at real fixture directories under `fixtures/` — no
`sudo` needed, plain TCP/MMS only):
- `test_scan_findsLiveServerAndSkipsUnreachableOne` — one live server (fixture
  `fixtures/served_files/`) plus one genuinely unroutable host
  (`10.255.255.1`, empirically confirmed unreachable in the sandboxed test environment) in the
  same `scanAndFetch` call; proves the live one returns `SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED`
  with byte-exact `fileData`/`fileSize` matching the on-disk `reporter1.cid`, and the unreachable
  one returns `SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER`, both in the correct input order.
- `test_scan_noServerListening_reportsNoMmsServer` — probing a dead port
  (10399, nothing bound) yields `SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER`.
- `test_scan_noSclFilePresent_reportsNotFound` — server up, filestore fixture
  (`fixtures/no_scl_files/`) contains only a `readme.txt`; yields
  `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND`.
- `test_scan_authRequiredNoPasswordConfigured_deniesAccess` — server requires auth
  (`SimServer_requireAuthentication(sim, "secret123")`, fixture `fixtures/auth_required/`), client
  handle has no `acseAuthPassword` configured; yields `SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED` with
  `authWasAttempted == false` (no password to retry with).
- `test_scan_authRequiredCorrectPassword_retrievesFile` — same auth-required server, client
  configured with the correct password; yields `SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED` with
  `authWasAttempted == true`, proving the unauthenticated-first-attempt-then-retry sequence
  actually round-trips end to end.
- `test_scan_authRequiredWrongPassword_deniesAccess` — same server, wrong password configured;
  yields `SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED` with `authWasAttempted == true` (the retry was
  attempted and also failed).

Together these six E2E cases prove every `SclBootstrapCandidateStatus` outcome except
`SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED` and `SCL_BOOTSTRAP_CANDIDATE_DOWNLOAD_FAILED` (no
fixture/scenario here forces either of those two specifically), plus the full password-auth-retry
symmetry (no-password / correct-password / wrong-password) and multi-host ordering. Fixture
files present on disk: `fixtures/served_files/reporter1.cid` (+ a nested
`fixtures/served_files/subdir/reporter1_extra.icd`, present but not directly asserted on by any
current test), `fixtures/no_scl_files/readme.txt`, `fixtures/auth_required/reporter1.cid`.
