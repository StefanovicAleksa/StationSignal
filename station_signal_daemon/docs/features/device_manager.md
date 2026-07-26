# device_manager

## 1. Overview

`src/device_manager/` is a top-level sibling of `src/features/`, `src/orchestration/`, and
`src/scan_orchestration/` — not itself a "feature" in this codebase's Expected-features sense.
It runs SEVERAL `orchestration` pipelines concurrently, one per physical IED, each with its own
auto-assigned `ipc_dispatcher` websocket port, addressable by a server-generated `deviceId`.
`src/orchestration/` itself has no idea multiple IEDs exist — one `OrchestrationHandle` wraps
exactly one IED's full MMS+GOOSE pipeline. This layer supplies the missing registry: deviceId
generation, per-device port allocation, running/mid-start bookkeeping, and duplicate-start
rejection. It has no protocol knowledge of its own (zero direct third-party includes — see
`domain/device_manager_types.h`'s own top comment) and no thread of its own; it is purely
concurrent-orchestration bookkeeping.

It exists because the feature layer below it (`orchestration`) is deliberately single-IED and
stateless-at-rest — nothing in `src/features/` or `src/orchestration/` keeps a global/static
registry of "which devices are currently reporting." `device_manager` is that registry.
`control_dispatcher` is the other missing piece — the control-plane transport (websocket JSON
commands) that actually invokes `DeviceManager_startReporting`/`_stopReporting` from the
outside world; `device_manager` itself has no network-facing control surface, it's a plain C
library. Public boundary: `src/device_manager/service/device_manager_api.h`. Callers (`main.c`,
`control_dispatcher`) must only ever include this header — never reach into `domain/`/`data/`
directly.

## 2. Public API surface

All declared in `src/device_manager/service/device_manager_api.h`.

- **`void DeviceManagerConfig_defaults(DeviceManagerConfig* config)`** — fills
  `wsPortRangeStart=9000`, `wsPortRangeEnd=9999`. Caller may override before
  `DeviceManager_create`.

- **`DeviceManagerHandle DeviceManager_create(const DeviceManagerConfig* config, DeviceManagerError* outError)`**
  — allocates only (registry + its own port allocator + mutex), no I/O. `config == NULL` means
  `DeviceManagerConfig_defaults()`. Returns NULL and sets `*outError` on an argument error
  (`wsPortRangeStart > wsPortRangeEnd`) or allocation failure only.

- **`DeviceManagerError DeviceManager_startReporting(DeviceManagerHandle handle, const char* host, int mmsPort, const char* iedName, const char* interfaceId, const char* sclFilePath, const char* acseAuthPassword, AccessMode accessMode, uint64_t* outDeviceId, uint16_t* outWsPort, DeviceManagerErrorDetail* outDetail)`**
  — blocking. Full contract:
  - **Argument validation first**, before touching the registry at all: `host` non-empty,
    `interfaceId` non-empty, and — per this feature's own explicit requirement, not
    `Orchestration_runFromLocalFile`'s own weaker contract — **`iedName` is mandatory whenever
    `sclFilePath` is given**. Any failure here returns `DEVICE_MANAGER_ERR_INVALID_ARGUMENT`
    immediately, before any reservation.
  - `sclFilePath`/`iedName`/`acseAuthPassword`: NULL or `""` both mean "not given" (optional).
    `iedName` empty is allowed only when `sclFilePath` is NOT given (falls through to
    `Orchestration_run`'s own auto-detect: exactly one `<IED>` in the fetched SCL).
  - **Phase A — reserve (short lock)**: reserves a `deviceId` + auto-assigned websocket port
    together. Returns `DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING` if this exact `(host, mmsPort)`
    is already running or mid-start — a default dedupe policy, not dictated by any original ask,
    trivially relaxable by deleting the check in `device_manager_registry.c` if a caller ever
    legitimately wants two independent handles against one device. Returns
    `DEVICE_MANAGER_ERR_PORT_EXHAUSTED` if the configured port range is fully allocated.
  - **Phase B — the slow part (NO lock held)**: builds an `OrchestrationConfig` (defaults,
    `ipcDispatcherConfig.port` = the reserved port, `bootstrapConfig.acseAuthPassword` /
    `reportClientConfig.acseAuthPassword` = the registry's own owned copy of
    `acseAuthPassword`, never the caller's borrowed pointer — see §3's registry breakdown for
    why), creates an `OrchestrationHandle`, and runs `DeviceManagerBootstrapPolicy_run` against
    it. Real network I/O, seconds-scale.
  - **Phase C — finalize or rollback (short lock)**: on success, attaches the handle to the
    reservation and returns `DEVICE_MANAGER_OK` with `outDeviceId`/`outWsPort` set. On failure,
    destroys the just-created `OrchestrationHandle` (outside any lock), then rolls back the
    reservation (frees the port and the owned host/password copies), returns
    `DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED` with `outDetail->orchestrationError`/
    `orchestrationDetail` set to whatever the failing `Orchestration_create`/
    `DeviceManagerBootstrapPolicy_run` call returned.
  - **`acseAuthPassword` is `strdup`'d into the registry entry** at reserve time — a
    control-plane message's source buffer is freed the moment that one request finishes
    processing, unlike `main.c`'s own argv (alive for the whole process), so
    `OrchestrationConfig`'s "caller keeps this alive for the handle's whole lifetime" contract
    would otherwise be violated the instant a `START_REPORTING` command arrives over
    `control_dispatcher`.

- **`DeviceManagerError DeviceManager_stopReporting(DeviceManagerHandle handle, uint64_t deviceId, DeviceManagerErrorDetail* outDetail)`**
  — blocking. Looks up `deviceId` (short lock, atomic remove — a concurrent duplicate stop for
  the same id always fails cleanly with `DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND` rather than
  double-tearing-down):
  - `DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND` if unknown/already stopped.
  - `DEVICE_MANAGER_ERR_START_IN_PROGRESS` if `deviceId` exists but is still mid-start on
    another thread — no cancellation hook exists; retry after a delay if "stop as soon as
    possible" is needed.
  - Otherwise: `Orchestration_stop` + `Orchestration_destroy` (NO lock held — same reverse-order
    teardown orchestration itself implements: goose → report client → ipc_dispatcher →
    ied_model), then frees this device's owned host/password copies and returns the port to the
    allocator (short lock). `outDetail` is optional (NULL-safe) — only the `DeviceManagerError`
    codes above are ever possible here, no `OrchestrationErrorDetail` to report
    (`orchestrationDetail` is left zero-valued).

- **`void DeviceManager_destroy(DeviceManagerHandle handle)`** — stops+destroys every still-running
  device by draining via the public `stopReporting` path one at a time (see
  `DeviceManagerRegistry_anyRunningDeviceId`'s own doc comment for the narrow
  mid-start-at-shutdown limitation — an in-flight start racing exactly with shutdown is not
  drained), then frees the registry and the handle. NULL-safe.

- **Duplicate-start rejection**: a `StartReporting` call for a `(host, mmsPort)` pair that is
  already running OR still mid-start (reserved but not yet finalized) is rejected with
  `DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING` — checked at reserve time, under the same lock that
  inserts the placeholder, so two concurrent starts against the same device can never both
  succeed.

## 3. Per-file breakdown

### `service/device_manager_api.h` / `device_manager_api.c` (176 lines)
The only public header; `.c` is orchestration-and-registry glue, no protocol logic of its own.
- `DeviceManagerConfig_defaults` — sets the `[9000, 9999]` default port range.
- `DeviceManager_create` — `calloc`s the handle, copies in config or defaults, validates
  `wsPortRangeStart <= wsPortRangeEnd`, creates the registry via `DeviceManagerRegistry_create`.
- `isEmpty`/`clearDetail` — small static helpers; `isEmpty` treats NULL and `""` identically
  throughout this file (the "not given" convention for optional params); `clearDetail` zeroes
  `outDetail->orchestrationError`/`orchestrationDetail` up front on both public entry points so a
  caller who doesn't check the return code still sees a clean/zeroed detail struct on success.
- `DeviceManager_startReporting` — implements the reserve → (no-lock) create+bootstrap-policy-run
  → finalize-or-rollback sequence described in §2. Notably: `orchestrationConfig.bootstrapConfig
  .acseAuthPassword` and `.reportClientConfig.acseAuthPassword` are both pointed at the SAME
  `ownedAcseAuthPassword` returned by `DeviceManagerRegistry_reserve` — one owned copy backs both
  config fields, borrowed for the `OrchestrationHandle`'s whole lifetime, freed only after that
  handle is torn down (rollback or stop).
- `DeviceManager_stopReporting` — implements the remove-if-running → (no-lock) stop+destroy →
  free-port sequence. The owned `host`/`acseAuthPassword` copies are freed here, in this
  function, only AFTER `Orchestration_destroy` has actually returned — freeing them any earlier
  would race the still-running `OrchestrationHandle`, which borrows those pointers.
- `DeviceManager_destroy` — loops `DeviceManagerRegistry_anyRunningDeviceId` → `DeviceManager_
  stopReporting` until nothing running remains, then destroys the registry and frees the handle.

### `domain/device_manager_types.h`
Pure data definitions, no behavior, zero direct third-party includes. Domain vocabulary is
entirely `orchestration`'s own public API (`OrchestrationHandle`/`OrchestrationError`/
`OrchestrationErrorDetail`, plus `AccessMode` transitively via `ied_model`'s public header) —
never `orchestration`'s own `domain`/`data` headers directly, the same rule `orchestration`
itself states about its relationship to `scl_bootstrap`/`ied_model`/etc. This layer's job is
running several `orchestration` pipelines concurrently and allocating each its own
`ipc_dispatcher` port — not talking to libiec61850/libwebsockets/cJSON itself.
- `DeviceManagerError` — `OK`, `ERR_INVALID_ARGUMENT`, `ERR_OUT_OF_MEMORY`,
  `ERR_PORT_EXHAUSTED` (configured `wsPortRange` fully allocated),
  `ERR_HOST_ALREADY_RUNNING` (this `(host, mmsPort)` already has a running or mid-start device),
  `ERR_ORCHESTRATION_FAILED` (see `DeviceManagerErrorDetail`), `ERR_DEVICE_NOT_FOUND`
  (`stopReporting`: unknown/already-stopped `deviceId`), `ERR_START_IN_PROGRESS`
  (`stopReporting`: `deviceId` exists but its `startReporting` call hasn't finished the slow
  phase yet on another thread — no cancellation hook, retry after a delay).
- `DeviceManagerErrorDetail` — `{ OrchestrationError orchestrationError; OrchestrationErrorDetail
  orchestrationDetail; }`, passed through verbatim from whichever `Orchestration_create`/
  `_run*` call failed. Mirrors this codebase's existing "status+detail two-part" convention
  (`SclBootstrapResult`, `OrchestrationErrorDetail` itself). Zero-valued/ignored for every other
  `DeviceManagerError`.
- `DeviceManagerConfig` — `{ uint16_t wsPortRangeStart; uint16_t wsPortRangeEnd; }` (defaults
  9000/9999, inclusive).
- `struct sDeviceManagerHandle` — `{ DeviceManagerConfig config; struct sDeviceManagerRegistry*
  registry; }` (owned). `DeviceManagerHandle` is a pointer to this struct.

### `domain/device_manager_bootstrap_policy.h` / `.c`
The shared "start a device" sequencing function. Extracted from `main.c`'s own
pre-`device_manager` sequencing so both `main.c`'s (now-removed) boot-time device and
`control_dispatcher`'s worker thread would share exactly one copy of this policy instead of
duplicating it. Zero third-party includes of its own — only calls `orchestration`'s own public
API (which itself transitively brings in `LinkedList`, used here only to satisfy
`Orchestration_run`'s own signature).

`DeviceManagerBootstrapPolicy_run(handle, host, mmsPort, iedName, interfaceId, sclFilePath,
acseAuthPassword, accessMode, outDetail)` → `OrchestrationError`:

1. **If `sclFilePath` is given**: calls `Orchestration_runFromLocalFile` directly and returns its
   result. `iedName` is validated as mandatory by the CALLER (`DeviceManager_startReporting`)
   before this function is ever reached — not re-validated here.
2. **Otherwise** (network SCL bootstrap path):
   - Wraps `host` in a single-element `LinkedList` (`LinkedList_create` + `LinkedList_add`), calls
     `Orchestration_run(handle, hostList, mmsPort, iedName, interfaceId, accessMode, outDetail)`,
     then `LinkedList_destroyStatic(hostList)` — `host` is caller-owned, not heap-owned by the
     list, so a static (non-deep) destroy is correct.
   - **One-shot online-discovery fallback**: if `Orchestration_run` returned exactly
     `ORCHESTRATION_ERR_BOOTSTRAP_FAILED` AND `outDetail->lastCandidateStatus ==
     SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` (a real, connectable device with no SCL file
     service — e.g. OMICRON IED Scout's "Simulate IED" mode), retries exactly once via
     `Orchestration_runFromOnlineDiscovery(handle, host, mmsPort, iedName, interfaceId,
     accessMode, acseAuthPassword, outDetail)` and returns THAT result instead. Any other failure
     mode (wrong port, no MMS server at all, access denied, etc.) is returned as-is with no
     retry. This is the concrete embodiment of the "No over-the-wire tree discovery" Hard Rule's
     one narrow exception — `ied_model_online_loader` is engaged only via this explicit,
     caller-invoked retry, never silently.

### `data/device_manager_registry.h` / `.c` (245 lines)
Mutex-guarded bookkeeping of every currently-starting-or-running device: `deviceId` generation,
the entries array, and the per-device websocket-port allocator
(`device_manager_port_allocator.h`). Opaque outside this file (`struct sDeviceManagerRegistry` is
defined only in the `.c`).

**Entry shape** (`DeviceManagerRegistryEntry`, private to the `.c`):
```c
typedef struct {
    uint64_t deviceId;
    uint16_t wsPort;
    bool running;                    /* false while still in the slow Orchestration_run* phase */
    char* host;                      /* owned copy - dedupe check + diagnostics only */
    int mmsPort;
    char* ownedAcseAuthPassword;     /* owned copy or NULL */
    OrchestrationHandle orchestrationHandle; /* NULL until the finalize phase attaches it */
} DeviceManagerRegistryEntry;
```
`struct sDeviceManagerRegistry` holds a `Semaphore lock` (`hal_thread.h` `Semaphore(1)` as a
binary mutex), an owned `entries` array (`count`/`capacity`, realloc-doubling from 8, swap-remove
on stop/rollback), a monotonic `nextDeviceId` starting at 1 (same convention as
`scan_orchestration`'s own `scanId`), and an owned `DeviceManagerPortAllocator`.

**THREE-PHASE LOCKING is the key design point** — the registry's own top comment spells out why:
`Orchestration_run` (any of its three variants) and `Orchestration_stop` can each block for
seconds (real network I/O — SCL bootstrap, MMS connect/disconnect), so this registry must NEVER
hold its lock across either call, or one device's slow start/stop would serialize every other
concurrent device's start/stop behind it.

- **Start needs a THIRD phase** (unlike `scan_orchestration`'s registry, see below): the
  `deviceId` + websocket port must be reserved (and a placeholder entry made visible to the
  host-dedupe check) BEFORE the slow `Orchestration_run*` call, so two concurrent starts never
  race for the same port or duplicate a host, and the reservation must be finalized (or rolled
  back) in a SEPARATE, later, short critical section once that slow call returns:
  ```
  reserve (short lock) -> Orchestration_create/_run* (NO lock) -> finalize or rollback (short lock)
  ```
- **Stop only needs the two phases** `scan_orchestration`'s own registry already has:
  `removeIfRunning` (short lock, atomic find-and-remove so a concurrent duplicate stop can never
  double-tear-down) → `Orchestration_stop`/`_destroy` (NO lock) → `freePort` (short lock).

**Compare/contrast with `scan_orchestration`'s two-phase registry**: `scan_orchestration_registry`
only ever needs "reserve a scanId (short lock) → do the slow work → done" for start, because a
scan's identity (its `scanId`) has nothing else contending for it — there's no equivalent of a
websocket port that must be provisionally claimed AND later confirmed/released depending on
outcome in the same way. `device_manager` additionally has to guard against two concurrent starts
racing for the same `(host, mmsPort)` pair or the same port-range slot, which is exactly why the
reserve phase here inserts a full placeholder entry (`running=false`) visible to
`hostAlreadyActive`'s dedupe scan, rather than just handing out an id.

**`acseAuthPassword` MUST be an OWNED copy**, never the caller's borrowed pointer:
`OrchestrationConfig` documents its own `acseAuthPassword` fields as "stays borrowed... caller
must keep it alive for the `OrchestrationHandle`'s whole lifetime" — true for `main.c`'s own
argv (alive for the whole process), but NOT true once a value arrives via a parsed control-plane
message, whose source buffer is freed the moment that one request finishes processing.
`DeviceManagerRegistry_reserve` therefore `strdup`s it immediately and hands back a BORROWED
pointer to its own copy (via `outOwnedAcseAuthPassword`) for the caller to wire straight into
`OrchestrationConfig` — the registry keeps ownership; the copy is freed only after
`Orchestration_destroy` actually runs (rollback or stop), never before.

Key functions:
- `DeviceManagerRegistry_create(wsPortRangeStart, wsPortRangeEnd)` — allocates the struct, the
  semaphore, and the port allocator; `nextDeviceId = 1`.
- `DeviceManagerRegistry_destroy` — does NOT stop/destroy any `OrchestrationHandle` still
  registered (caller, `DeviceManager_destroy`, must drain every running device via the public
  `stopReporting` path first). Only frees this registry's own bookkeeping (entries array, port
  allocator, owned host/password copies, lock). NULL-safe.
- `appendEntry` (static) — realloc-doubling insert (capacity 0→8→16→...).
- `hostAlreadyActive` (static) — linear scan over `entries` matching `(host via strcmp, mmsPort)`;
  expected to stay small (tens, not thousands, of concurrently reporting devices).
- `findIndexByDeviceId` (static) — linear scan by `deviceId`.
- `DeviceManagerRegistry_reserve` — PHASE A (start). Under lock: rejects with
  `DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING` if `hostAlreadyActive`; allocates a port via
  `DeviceManagerPortAllocator_alloc` (returns `DEVICE_MANAGER_ERR_PORT_EXHAUSTED` if none left);
  `strdup`s `host` and `acseAuthPassword` (via `OrchestrationUtils_safeStringDup` —
  `acseAuthPassword` may be NULL, meaning unauthenticated); on any allocation failure, frees
  everything already allocated and returns `DEVICE_MANAGER_ERR_OUT_OF_MEMORY`; otherwise appends
  a `running=false` placeholder entry with a freshly-issued `deviceId` (`nextDeviceId++`).
  `*outOwnedAcseAuthPassword` is set to a BORROWED pointer to the registry's own copy (NULL if
  `acseAuthPassword` was NULL). On any error path, every out-param is left untouched.
- `DeviceManagerRegistry_finalize` — PHASE C-success (start). Finds `deviceId`'s placeholder,
  attaches the now-successfully-created `OrchestrationHandle`, flips `running=true`. No-op if
  `deviceId` isn't found (should not happen in practice — only this handle's own `reserve` call
  ever creates it).
- `DeviceManagerRegistry_rollback` — PHASE C-failure (start). Finds `deviceId`'s placeholder,
  frees its port back to the allocator, frees the owned host/password copies, swap-removes the
  entry (order among active devices is never meaningful). Caller must have already called
  `Orchestration_destroy` on the failed handle OUTSIDE any lock before calling this — this
  function never touches the handle itself.
- `DeviceManagerRegistry_removeIfRunning` — PHASE A (stop). Atomic find-and-remove:
  `DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND` if no entry has this id; `DEVICE_MANAGER_ERR_
  START_IN_PROGRESS` if the entry exists but is still mid-start (`running==false`) on another
  thread — no cancellation hook exists, same accepted limitation `ScanOrchestrationWorker_stop`
  already documents for its own in-flight sweep. Otherwise removes the entry right here (a
  concurrent duplicate stop for the same `deviceId` can never find it again) and hands back the
  owned handle/port/host/password (as owned pointers — caller now owns host/password and must
  free them, only AFTER `Orchestration_destroy` has actually run) for the caller to tear down
  OUTSIDE the lock. `outOwnedHost`/`outOwnedAcseAuthPassword` are NULL-safe — passing NULL frees
  that copy immediately instead of handing it back.
- `DeviceManagerRegistry_freePort` — PHASE C (stop). Returns the port to the allocator's
  free-list under lock.
- `DeviceManagerRegistry_anyRunningDeviceId` — returns any one currently RUNNING (not mid-start)
  `deviceId`, used only by `DeviceManager_destroy` to drain the registry one device at a time via
  the public `stopReporting` path. Returns 0 if none (0 is never a valid `deviceId`). A device
  still mid-start at destroy time (an extremely narrow race — a concurrent `StartReporting` call
  landing exactly during process shutdown) is not returned here and is an accepted, undrained
  limitation of this v1.

### `data/device_manager_port_allocator.h` / `.c`
Simple range + free-list bookkeeping for device_manager's per-device `ipc_dispatcher` websocket
ports — explicit bookkeeping, no bind-and-retry probing (matches this codebase's preference for
explicit accounting over speculative I/O). **NOT thread-safe on its own** — always called under
`device_manager_registry`'s own lock, same as `scan_orchestration_registry.c` folds its own
`nextScanId` counter into its own lock rather than giving it an independent one. Opaque outside
this file.

`struct sDeviceManagerPortAllocator`: `{ uint16_t rangeStart; uint16_t rangeEnd; uint32_t
nextUnissued; uint16_t* freeList; int freeCount; int freeCapacity; }` — `nextUnissued` is
deliberately `uint32_t`, not `uint16_t`, so it can go one past a `rangeEnd` of `UINT16_MAX`
without wrapping back to 0 (which would otherwise falsely look like the range isn't exhausted
yet).

- `DeviceManagerPortAllocator_create(rangeStart, rangeEnd)` — NULL if `rangeStart > rangeEnd` or
  allocation fails; `nextUnissued = rangeStart`.
- `DeviceManagerPortAllocator_destroy` — frees the free-list array and the struct. NULL-safe.
- `DeviceManagerPortAllocator_alloc(allocator, outPort)` — **pops the free-list first** if
  non-empty (reuse-after-free preferred over growing the never-yet-issued counter, keeps the
  allocated range compact); otherwise issues `nextUnissued` and increments it. Returns `false`
  (`*outPort` left untouched) once `nextUnissued > rangeEnd` AND the free-list is empty — i.e.
  every port in `[rangeStart, rangeEnd]` is simultaneously allocated. Default range is
  `[9000, 9999]` (1000 ports → 1000 concurrent devices max under defaults).
- `DeviceManagerPortAllocator_free(allocator, port)` — pushes onto the free-list, realloc-doubling
  from capacity 0→8→16→... . If that realloc fails, the port silently leaks permanently out of
  the range — an accepted OOM edge case, explicitly commented in the source.

## 4. Threading & concurrency model

`device_manager` is **a synchronous library, no thread of its own** — `DeviceManager_
startReporting`/`_stopReporting` each block the calling thread for as long as the underlying
`Orchestration_run*`/`Orchestration_stop` call takes, but never serialize behind a DIFFERENT
device's own slow call. The registry's three-phase (start) / two-phase (stop) locking design in
§3 is exactly how this is achieved:

- The `Semaphore lock` is held ONLY during the short, in-memory bookkeeping phases — reserve,
  finalize, rollback, removeIfRunning, freePort — each a handful of array/list operations, never
  during `Orchestration_create`, `DeviceManagerBootstrapPolicy_run` (SCL bootstrap, MMS connect,
  GOOSE subscribe — seconds-scale real network I/O), `Orchestration_stop`, or
  `Orchestration_destroy`.
- Two threads calling `DeviceManager_startReporting` for two different devices concurrently: both
  briefly contend for the lock during their own reserve phase (sub-millisecond each), then run
  their own slow `Orchestration_run*` fully in parallel with no shared lock held at all. Thread A
  finishing its slow phase and taking the lock again for `finalize` cannot block on Thread B still
  being mid-bootstrap — they simply don't share a critical section during the slow part.
  `integration_tests/device_manager/e2e_test_device_manager.c` proves exactly this against two
  real `ied_simulator` instances (see §7).
- A `StartReporting` and a `StopReporting` for two DIFFERENT devices similarly never block on each
  other's slow phase, for the same reason — each only holds the lock for its own short
  bookkeeping phases.
- A `StartReporting` and a `StopReporting` for the SAME `deviceId` can't race destructively: while
  a start is mid-flight (reserved but not finalized), the placeholder entry has `running=false`,
  so a concurrent stop attempt for that same id gets `DEVICE_MANAGER_ERR_START_IN_PROGRESS`
  rather than tearing down a handle that doesn't exist yet or double-freeing anything.
- `DeviceManager_destroy` drains sequentially — one `stopReporting` call at a time via
  `anyRunningDeviceId` — so multiple devices' teardowns during shutdown are NOT parallelized; this
  is a deliberate simplicity tradeoff for a one-shot shutdown path, not a hot path.
- No condition variables/signaling anywhere in this layer — every call is a plain blocking
  function call; whatever thread invokes `DeviceManager_startReporting`/`_stopReporting` is
  blocked for the operation's whole real-world duration. `control_dispatcher` is the caller that
  actually puts these calls on a dedicated worker thread so its own lws service thread never
  blocks — see that feature's own Architecture bullet in CLAUDE.md.

## 5. Known limitations / deliberate scope boundaries

- **Does NOT watch connection health or auto-stop a device on connection loss** — only an explicit
  `STOP_REPORTING` (via `DeviceManager_stopReporting`) tears one down. There is no reaper, no
  liveness thread, no automatic deregistration on an `IedConnection` going down underneath a
  running device.
- **No cancellation hook for an in-flight start** — `DEVICE_MANAGER_ERR_START_IN_PROGRESS` is the
  only signal a caller gets that a stop landed mid-start; the caller's only option is to retry
  the stop after a delay once the start actually finishes (success or failure).
- **`DeviceManager_destroy` does not drain a device that is mid-start exactly at shutdown time** —
  `DeviceManagerRegistry_anyRunningDeviceId` only ever returns `running==true` entries; an
  extremely narrow race (a `StartReporting` call landing exactly during process shutdown) leaves
  that device's `OrchestrationHandle` — once it eventually finalizes — un-torn-down by this path.
  Accepted v1 limitation.
- **Host-dedupe policy is a default, not a hard requirement** — rejecting a second
  `StartReporting` for an already-active `(host, mmsPort)` is this feature's own choice (§2),
  trivially relaxable by deleting the `hostAlreadyActive` check in
  `device_manager_registry.c` if a caller ever legitimately wants two independent handles
  against the same device.
- **A port-allocator `free()` that hits OOM during its free-list realloc leaks that one port
  permanently** out of the configured range for the rest of the process's life — accepted edge
  case, explicitly commented in `device_manager_port_allocator.c`.
- **Default port range is 1000 wide (`[9000, 9999]`)** — a hard ceiling on concurrently
  reporting devices under default config; raising it is a config-only change
  (`DeviceManagerConfig.wsPortRangeStart`/`End`), not a code change.
- **`DeviceManagerRegistry_removeIfRunning`'s linear scans (`hostAlreadyActive`,
  `findIndexByDeviceId`) are O(n) over active devices** — accepted, "expected to stay small (tens,
  not thousands)" per the registry's own doc comments; not a concern at this scale, would need
  revisiting if the daemon were ever expected to manage thousands of concurrent devices.

## 6. Cross-feature dependencies

**Wraps:**
- `src/orchestration/` — one `OrchestrationHandle` instance per device, created fresh on every
  `DeviceManager_startReporting` call and destroyed on the matching `_stopReporting` (or drained
  by `DeviceManager_destroy`). `device_manager` never reaches into `orchestration`'s own
  `domain`/`data`/`utils` — only its public `service/orchestration_api.h`, which itself
  transitively owns `ipc_dispatcher`'s, `mms_report_client`'s, and `goose_subscriber`'s entire
  lifecycles for that one IED (see `orchestration`'s own Architecture bullet).
- `DeviceManagerBootstrapPolicy_run` calls `Orchestration_runFromLocalFile`, `Orchestration_run`,
  and — as the one-shot fallback — `Orchestration_runFromOnlineDiscovery` (§3), never
  `ied_model_online_loader` or `scl_bootstrap` directly; those stay entirely inside
  `orchestration`'s own sequencing.

**Called by:**
- `control_dispatcher` — the daemon's only control-plane transport. Its `START_REPORTING`/
  `STOP_REPORTING` JSON commands are relayed straight to `DeviceManager_startReporting`/
  `_stopReporting` from a dedicated worker thread (off the lws service thread — see
  `control_dispatcher`'s own Architecture bullet in CLAUDE.md), so the slow, blocking nature of
  these two calls never stalls the control websocket's own frame handling.
- `main.c` — creates and owns the one `DeviceManagerHandle` for the daemon's whole lifetime
  (alongside `scan_orchestration` and `control_dispatcher`), destroys it in reverse order on
  `SIGINT`/`SIGTERM`. `main.c` never calls `DeviceManager_startReporting`/`_stopReporting`
  directly itself — there is no boot-time device; every device lifecycle action goes exclusively
  through `control_dispatcher`'s commands.

**Does NOT depend on:**
- `scan_orchestration`/`ied_discovery`/`scan_dispatcher` — device discovery and device reporting
  are entirely separate concerns in this codebase; `device_manager` only ever reports on a
  `(host, mmsPort)` it's explicitly told about, never one it discovered itself.
- libiec61850/libwebsockets/cJSON or any other third-party library directly — `device_manager`'s
  own domain vocabulary is exclusively `orchestration`'s public API (§3).

## 7. Tests

**`tests/device_manager/`** (Unity unit tests, hermetic, wired into `tests/Makefile`'s explicit
`TESTS` list as `test_device_manager_port_allocator`/`test_device_manager_registry`/
`test_device_manager_api`) — covers port allocation and the registry against fake/no-op
`OrchestrationHandle` values, **never a real `Orchestration_run*` call**:

- `test_device_manager_port_allocator.c` — pure allocator logic, no registry involved:
  `test_create_rejectsInvertedRange`, `test_create_acceptsSinglePortRange`,
  `test_alloc_issuesSequentially` (sequential issuance from `rangeStart`),
  `test_alloc_exhaustion_returnsFalse` (every port in range allocated → `false`),
  `test_free_thenAlloc_reusesFreedPort_beforeGrowingRange` (free-list popped before
  `nextUnissued` grows), `test_alloc_exhaustionThenFree_recovers` (exhausted → free one → alloc
  succeeds again), `test_nullSafety_doesNotCrash`.

- `test_device_manager_registry.c` — the three/two-phase locking contract itself, against
  in-memory state only:
  `test_reserve_issuesMonotonicDeviceIdsAndDistinctPorts`,
  `test_reserve_rejectsDuplicateHostAndPort` (the `HOST_ALREADY_RUNNING` dedupe check),
  `test_reserve_allowsSameHostOnDifferentPort` (dedupe key is `(host, mmsPort)`, not `host`
  alone), `test_reserve_returnsBorrowedOwnedPasswordCopy_distinctFromCallersBuffer` (proves the
  `strdup`-not-borrow contract — mutating the caller's own password buffer after `reserve`
  doesn't affect the registry's copy), `test_reserve_portExhaustion`,
  `test_removeIfRunning_returnsStartInProgress_beforeFinalize` (a reserved-but-not-finalized
  entry can't be stopped), `test_finalize_thenRemoveIfRunning_returnsHandleAndPort` (the happy
  path: reserve → finalize → removeIfRunning hands back the same handle/port),
  `test_removeIfRunning_isNotFound_onSecondCall_noDoubleTeardown` (proves the atomic
  find-and-remove — a second stop for the same id can never re-find it),
  `test_removeIfRunning_unknownDeviceId_returnsNotFound`, `test_rollback_freesPortForReuse`,
  `test_anyRunningDeviceId_isZero_whenNothingFinalized`,
  `test_anyRunningDeviceId_returnsFinalizedDevice`, `test_freePort_thenReserve_reusesIt`,
  `test_nullSafety_doesNotCrash`.

- `test_device_manager_api.c` — the public API's argument validation and structural behavior,
  against a handle with no devices ever actually started (build rule pulls in the full
  `device_manager` stack plus orchestration's entire stack, per `tests/Makefile`'s own comment,
  but no test here drives a real `Orchestration_run*`):
  `test_configDefaults_matchDocumentedValues`, `test_configDefaults_doesNotCrash_onNull`,
  `test_create_appliesDefaults_whenConfigIsNull`, `test_create_rejectsInvertedPortRange`,
  `test_startReporting_rejectsNullHandle`, `test_startReporting_rejectsEmptyHost`,
  `test_startReporting_rejectsEmptyInterfaceId`,
  `test_startReporting_rejectsSclFilePath_withoutIedName` (the mandatory-iedName-with-
  sclFilePath contract from §2), `test_startReporting_rejectsNullOutParams`,
  `test_stopReporting_rejectsNullHandle`,
  `test_stopReporting_returnsDeviceNotFound_forUnknownId`,
  `test_destroy_doesNotCrash_onNullHandle`,
  `test_destroy_onFreshHandle_withNoDevices_doesNotCrash`.

**`integration_tests/device_manager/`** (`e2e_test_device_manager.c`, needs `sudo` — inherits
`CAP_NET_RAW` transitively from the GOOSE subscriber step every `DeviceManager_startReporting`
call reaches via `orchestration`) — drives the REAL `DeviceManager_startReporting`/
`_stopReporting` against TWO real "Reporter1" `ied_simulator` instances running in-process over
loopback at two different `mmsPort`s (`TEST_PORT_A=10501`, `TEST_PORT_B=10502`, `interfaceId=
"lo"`, device-manager ws port range `[19500, 19599]`):

- `test_twoConcurrentDevices_independentPortsAndReporting_thenStopAndPortReuse` — the one test in
  this suite, proving three things end-to-end:
  1. Two concurrent `StartReporting` calls (driven from two threads) don't serialize behind each
     other's slow phase — the two/three-phase-locked registry's whole reason for existing. The
     timing bound used is deliberately coarse/non-flaky (the registry's own unit tests already
     prove the lock is structurally released before the slow call; this just proves it holds up
     against two REAL bootstrap+MMS+GOOSE sequences too).
  2. Each device gets a distinct `deviceId` and a distinct, real, independently-connectable
     `ipc_dispatcher` websocket, each streaming real report/GOOSE JSON for its own simulator
     (`EXPECTED_GOCB_REF = "Reporter1LD1/LLN0$GO$gcbInd"`).
  3. Stopping one device leaves the other running; stopping the second frees its port for reuse
     by a subsequent start.

Neither suite exercises `DeviceManagerBootstrapPolicy_run`'s online-discovery fallback branch
directly (that path is proven in `integration_tests/orchestration/`'s own
`test_onlineDiscoveryFallback_afterNoSclFileFound_endToEnd` instead, against
`Orchestration_runFromOnlineDiscovery` directly) — `device_manager`'s own E2E coverage is scoped
to proving concurrent multi-device sequencing and registry correctness under real orchestration
calls, not re-proving orchestration's own fallback logic.
