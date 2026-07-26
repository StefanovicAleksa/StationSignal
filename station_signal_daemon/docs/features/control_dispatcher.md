# `control_dispatcher`

> Source: `src/features/control_dispatcher/`

---

## 1. Overview

`control_dispatcher` is the daemon's **only external interface**. `main.c` takes zero
arguments and has no CLI/terminal surface — every device/scan lifecycle action (start
reporting on an IED, stop it, start a subnet scan, stop it) goes exclusively through this
one bidirectional websocket. It binds `127.0.0.1:8767` (no TLS, loopback-only), RECEIVES
JSON commands (`START_REPORTING` / `STOP_REPORTING` / `START_SCAN` / `STOP_SCAN`), and
pushes back JSON acks/errors on the same connection.

This is the first bidirectional websocket in the codebase. `ipc_dispatcher` (per-device
MMS/GOOSE report stream, port 8765) and `scan_dispatcher` (per-scan discovery stream, port
8766) are both push-only — a client connects and reads, never writes. `control_dispatcher`
adds inbound frame handling on top of the same broadcast-ring-buffer/libwebsockets-service-
thread transport those two already use, and relays parsed commands to `device_manager`
(`START_REPORTING`/`STOP_REPORTING`) and `scan_orchestration` (`START_SCAN`/`STOP_SCAN`) —
the daemon's two top-level orchestration registries, both created once by `main.c` and kept
alive for the daemon's whole lifetime.

Public boundary: `src/features/control_dispatcher/service/control_dispatcher_api.h`.

---

## 2. Public API surface

```c
void ControlDispatcherConfig_defaults(ControlDispatcherConfig* config);

ControlDispatcherHandle ControlDispatcher_create(const ControlDispatcherConfig* config,
        DeviceManagerHandle deviceManager, ScanOrchestrationHandle scanOrchestration,
        ControlDispatcherError* outError);

ControlDispatcherError ControlDispatcher_start(ControlDispatcherHandle handle);
void ControlDispatcher_stop(ControlDispatcherHandle handle);
void ControlDispatcher_destroy(ControlDispatcherHandle handle);
```

- **`ControlDispatcherConfig_defaults`** — `port=8767` (next after `ipc_dispatcher`'s 8765
  and `scan_dispatcher`'s 8766), `ringBufferCapacity=256`, `maxConnections=16`,
  `requestQueueCapacity=64`. Caller may override individual fields before `_create`.
- **`ControlDispatcher_create`** — allocation only (ring buffer + request queue, both pure
  allocation): no bind, no thread yet, matching every sibling feature's "no I/O at create"
  contract. `config == NULL` applies the defaults above. Rejects `NULL` `deviceManager` or
  `scanOrchestration` and non-positive capacities with `CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT`.
  **`deviceManager`/`scanOrchestration` are BORROWED** — `main.c` owns their lifetime and
  must keep both alive for at least this handle's own lifetime (create..destroy). This
  feature never creates or destroys either.
- **`ControlDispatcher_start`** — binds `127.0.0.1:config.port`, starts the libwebsockets
  service thread AND the dedicated request-worker thread. Non-blocking once the synchronous
  bind succeeds. `CONTROL_DISPATCHER_ERR_ALREADY_RUNNING` if called twice without an
  intervening `_stop()`. `CONTROL_DISPATCHER_ERR_SOCKET_BIND_FAILED` if the port is
  unavailable — the worker thread is never started in that case (bind-first, fail-fast,
  same reasoning as `ipc_dispatcher`/`orchestration`'s own first stage).
- **`ControlDispatcher_stop`** — stops the worker thread first (discards anything still
  queued, no graceful drain — matches `IpcDispatcher_stop`'s own behavior), then the ws
  server (fully tears down the lws context, so a subsequent `_start` on the same handle can
  cleanly rebind the same port). Blocking. Must be called from the caller's own thread,
  never from within a callback (deadlock). Safe to call more than once / on a never-started
  handle (no-op).
- **`ControlDispatcher_destroy`** — implies `_stop()` if still running, frees the handle
  (ring buffer + request queue). Does NOT touch the borrowed `DeviceManagerHandle`/
  `ScanOrchestrationHandle`. NULL-safe.

---

## 3. Per-file breakdown

### `service/control_dispatcher_api.h` / `.c`

Wires the four sub-components together and owns their lifetime resolution:

- `ControlDispatcher_create` allocates the ring buffer and request queue (both borrowed
  downward into every other component) — no thread, no bind.
- `ControlDispatcher_start` resolves a mutual-dependency ordering problem: the ws server
  needs to exist before the worker can be created (worker needs a `wsServer` handle to wake
  after processing), but the ws server's `onRequestQueued` callback needs the worker (to
  post its wake semaphore). Resolved by creating the ws server first with a `NULL` callback,
  creating the worker with a reference to that already-created ws server, then calling
  `ControlDispatcherWsServer_setRequestQueuedCallback` to wire the worker's
  `ControlDispatcherWorker_notify` in via a `void*`-erased trampoline
  (`onRequestQueuedTrampoline`). Order of teardown-on-failure mirrors this: ws server
  destroyed if worker creation fails, both destroyed if either `_start` call fails.
- `ControlDispatcher_stop`/`_destroy` tear down worker-then-ws-server (see §2 above for the
  ordering rationale).

### `domain/control_dispatcher_types.h`

Domain vocabulary for the whole feature — includes `device_manager_api.h` and
`scan_orchestration_api.h` directly (this feature's whole job is relaying calls into both;
depending on both top-level siblings is the point, not a layering violation — see §6).

- **`ControlDispatcherError`** — `OK`, `ERR_INVALID_ARGUMENT`, `ERR_OUT_OF_MEMORY`,
  `ERR_THREAD_CREATE_FAILED`, `ERR_SOCKET_BIND_FAILED`, `ERR_ALREADY_RUNNING`.
- **`ControlDispatcherConfig`** — `port` (uint16_t), `ringBufferCapacity`, `maxConnections`,
  `requestQueueCapacity` (all int). One shared control channel, not per-device — unlike
  `ipc_dispatcher`'s per-device report websockets.
- **`ControlRequestType`** — `CONTROL_REQ_START_REPORTING`, `CONTROL_REQ_STOP_REPORTING`,
  `CONTROL_REQ_START_SCAN`, `CONTROL_REQ_STOP_SCAN`.
- **`ControlRequest`** — one fully-parsed inbound command, queued from the lws thread to the
  worker thread. Every string field is an owned copy (the raw JSON buffer is freed
  immediately after parsing, inside the lws callback). Fields:
  - `requestId` (owned, never NULL once parsed) — echoed back verbatim in every response
    derived from this request.
  - `type` (`ControlRequestType`).
  - START_REPORTING fields (unused/zero otherwise): `host`, `mmsPort`, `iedName` (owned, may
    be NULL — auto-detect), `interfaceId`, `sclFilePath` (owned, may be NULL), 
    `acseAuthPassword` (owned, may be NULL), `accessMode`.
  - STOP_REPORTING field: `deviceId` (uint64_t).
  - START_SCAN fields (plus `interfaceId`/`mmsPort` above, shared with START_REPORTING since
    the two request types are mutually exclusive per message): `sweepIntervalMs` (uint32_t,
    0 = `ScanOrchestration`'s own default).
  - STOP_SCAN field: `scanId` (uint64_t).
- **`struct sControlDispatcherHandle`** — `config`, owned `ringBuffer`/`requestQueue`
  (created in `_create`), owned `wsServer`/`worker` (created/destroyed in `_start`/`_stop`,
  `NULL` when not running), borrowed `deviceManager`/`scanOrchestration`, `volatile bool
  running`.
- Opaque forward declarations only for `sControlDispatcherRingBuffer`/`sControlDispatcherWsServer`/
  `sControlDispatcherRequestQueue`/`sControlDispatcherWorker` — kept as bare tags so this
  header never pulls in `hal_thread.h`/`libwebsockets.h`.

### `domain/control_dispatcher_usecases.c` / `.h`

The one function the worker thread calls per popped request:
`ControlDispatcherUseCases_processRequest(request, deviceManager, scanOrchestration)`.
Dispatches on `request->type`, calls the matching `DeviceManager_*`/`ScanOrchestration_*`
function, and builds the resulting JSON response via `control_dispatcher_json_writer`.
Returns an owned heap string (caller pushes it onto the ring buffer), or `NULL` only on
cJSON allocation failure (response silently dropped — no queue to retry onto). This is the
**only** place in the feature that maps `DeviceManagerError`/`ScanOrchestrationError` to this
feature's stable string error codes:

**`DeviceManagerError` → code / message:**

| Enum | code | message |
|---|---|---|
| `DEVICE_MANAGER_OK` | `OK` | — |
| `DEVICE_MANAGER_ERR_INVALID_ARGUMENT` | `INVALID_ARGUMENT` | "invalid argument" |
| `DEVICE_MANAGER_ERR_OUT_OF_MEMORY` | `OUT_OF_MEMORY` | "out of memory" |
| `DEVICE_MANAGER_ERR_PORT_EXHAUSTED` | `PORT_EXHAUSTED` | "no free websocket port left in the configured range" |
| `DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING` | `HOST_ALREADY_RUNNING` | "this host/mmsPort is already running or starting" |
| `DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED` | `ORCHESTRATION_FAILED` | "orchestration failed" |
| `DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND` | `DEVICE_NOT_FOUND` | "unknown or already-stopped deviceId" |
| `DEVICE_MANAGER_ERR_START_IN_PROGRESS` | `START_IN_PROGRESS` | "device is still starting on another request - retry shortly" |
| (any other/unknown) | `UNKNOWN_ERROR` | "unknown device_manager error" |

For `DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED` specifically, `error.stage` is populated from
`OrchestrationUtils_stageToString(detail->orchestrationDetail.stage)`, and — only when that
stage is `ORCHESTRATION_STAGE_BOOTSTRAP` — `error.detail` is additionally populated from
`OrchestrationUtils_candidateStatusToString(detail->orchestrationDetail.lastCandidateStatus)`.
Every other error code leaves both `stage`/`detail` unset (omitted from the JSON entirely).

**`ScanOrchestrationError` → code / message:**

| Enum | code | message |
|---|---|---|
| `SCAN_ORCHESTRATION_OK` | `OK` | — |
| `SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT` | `INVALID_ARGUMENT` | "invalid argument (check interfaceId/mmsPort)" |
| `SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY` | `OUT_OF_MEMORY` | "out of memory" |
| `SCAN_ORCHESTRATION_ERR_DISPATCHER_START_FAILED` | `DISPATCHER_START_FAILED` | "scan_dispatcher websocket failed to bind" |
| `SCAN_ORCHESTRATION_ERR_THREAD_CREATE_FAILED` | `THREAD_CREATE_FAILED` | "failed to start the scan's worker thread" |
| `SCAN_ORCHESTRATION_ERR_DISCOVERY_CREATE_FAILED` | `DISCOVERY_CREATE_FAILED` | "failed to create the underlying discovery handle" |
| `SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND` | `SCAN_NOT_FOUND` | "unknown or already-stopped scanId" |
| (any other/unknown) | `UNKNOWN_ERROR` | "unknown scan_orchestration error" |

`ScanOrchestrationError` responses never populate `stage`/`detail` (both always `NULL`).

`CONTROL_REQ_START_SCAN` handling builds a `ScanRequest` from `request->interfaceId`/
`mmsPort`/`sweepIntervalMs`, with `acseAuthPassword` hardcoded to `NULL` — control-triggered
scans are deliberately unauthenticated (not part of the original ask; `ScanRequest` already
has its own per-scan field if this needs extending).

### `data/control_dispatcher_json_parser.c` / `.h` (221 lines)

The **first `cJSON_Parse` call in this codebase's production code**. Every field is
defensively type/NULL-checked before use — untrusted network input, unlike every other cJSON
use in this repo (which only ever serializes trusted, internally-generated data).

**`ControlParseError`** enum:

| Value | Meaning |
|---|---|
| `CONTROL_PARSE_OK` | — |
| `CONTROL_PARSE_ERR_MALFORMED_JSON` | `cJSON_Parse` itself returned NULL, or the root wasn't a JSON object |
| `CONTROL_PARSE_ERR_MISSING_REQUEST_ID` | `"requestId"` missing, not a string, or empty |
| `CONTROL_PARSE_ERR_UNKNOWN_ACTION` | `"action"` missing, not a string, or not one of the four known action names |
| `CONTROL_PARSE_ERR_INVALID_PARAMS` | action recognized, but `"params"` is missing/not an object, or a required field within it is missing/wrong-typed/empty |

**`ControlDispatcherJsonParser_parse(rawJson, len, outRecoveredRequestId, outRecoveredAction,
outRequest)`** — parses one raw inbound websocket text frame:

1. `cJSON_ParseWithLength` the buffer; not an object → `MALFORMED_JSON`.
2. Best-effort recover `requestId`/`action` into the two `out*` params **regardless of
   ultimate success/failure** (used to echo them back on the error path — see the ws_server's
   `handleCompleteMessage`). `requestId` recovery is checked first; missing/empty →
   `MISSING_REQUEST_ID`.
3. `action` string-matched against `"START_REPORTING"`/`"STOP_REPORTING"`/`"START_SCAN"`/
   `"STOP_SCAN"`; anything else (including missing) → `UNKNOWN_ACTION`.
4. `"params"` must be a present JSON object → else `INVALID_PARAMS`.
5. Allocates the `ControlRequest`, dispatches to one of four per-action parameter parsers.
6. On success, frees the two recovered strings (the request itself now carries `requestId`;
   `action` is implied by `request->type`) and hands back the owned `ControlRequest*`.

Per-action parsing:

- **`parseStartReportingParams`** — `host` (required, non-empty string), `interfaceId`
  (required, non-empty string), `mmsPort` (optional int, default `102`), `iedName` (optional
  string, `NULL` = auto-detect), `sclFilePath` (optional string), `acseAuthPassword`
  (optional string), `accessMode` (optional string — `"REPORT_ONLY"` default if absent,
  otherwise must be exactly `"REPORT_ONLY"`/`"READ_ONLY"`/`"READ_AND_WRITE"` or the whole
  parse fails). **`sclFilePath` given without `iedName` fails fast here** with
  `INVALID_PARAMS` — `device_manager` also validates this, but failing at parse time gives a
  clearer error than reaching `device_manager` only to be rejected there too.
- **`parseStopReportingParams`** — `deviceId` (required, non-negative number).
- **`parseStartScanParams`** — `interfaceId` (required, non-empty string), `mmsPort`
  (optional int, default `102`), `sweepIntervalMs` (optional int; non-positive values
  collapse to `0`, meaning "use `ScanOrchestration`'s own default").
- **`parseStopScanParams`** — `scanId` (required, non-negative number).

`dupOptionalString` treats a present-but-JSON-`null` value identically to an absent key (both
mean "not given") — the two are deliberately indistinguishable here.

`isNonEmptyString` requires the value to be a `cJSON` string type AND have a non-empty
`valuestring` — an empty string (`""`) is treated the same as absent for every required
string field.

### `data/control_dispatcher_json_writer.c` / `.h`

Builds the outbound envelope — the stable JSON contract every response follows:

```json
{ "schemaVersion": 1, "requestId": "...", "action": "...", "success": true|false,
  "result": {...} | null, "error": {...} | null }
```

`requestId`/`action` are written as JSON `null` (not omitted) when the inbound message
couldn't be parsed far enough to recover them (e.g. genuinely malformed JSON with no
recoverable `requestId`).

Five builder functions, each returning an owned heap string (or `NULL` only on cJSON
allocation failure):

- `ControlDispatcherJsonWriter_writeStartSuccess(requestId, deviceId, wsPort)` — `action:
  "START_REPORTING"`, `success: true`, `result: {deviceId, wsPort}`, `error: null`.
- `ControlDispatcherJsonWriter_writeStopSuccess(requestId, deviceId)` — `action:
  "STOP_REPORTING"`, `result: {deviceId}`.
- `ControlDispatcherJsonWriter_writeScanStartSuccess(requestId, scanId)` — `action:
  "START_SCAN"`, `result: {scanId}`.
- `ControlDispatcherJsonWriter_writeScanStopSuccess(requestId, scanId)` — `action:
  "STOP_SCAN"`, `result: {scanId}`.
- `ControlDispatcherJsonWriter_writeError(requestId, action, errorCode, message, stage,
  detail)` — `success: false`, `result: null`, `error: {code, message, stage?, detail?}`.
  `stage`/`detail` are `NULL`-omittable (key entirely absent from the JSON, not written as
  `null`) — only populated for `ORCHESTRATION_FAILED` per the usecases mapping above.
  `errorCode` defaults to `"UNKNOWN_ERROR"` and `message` to `""` if passed `NULL`. `action`
  may be `NULL` (unknown/unrecoverable action) and is then written as JSON `null` via the
  shared `newEnvelope` helper.

Stable error-code strings seen across the whole feature (parser + usecases combined):
`INVALID_ARGUMENT`, `OUT_OF_MEMORY`, `PORT_EXHAUSTED`, `HOST_ALREADY_RUNNING`,
`ORCHESTRATION_FAILED`, `DEVICE_NOT_FOUND`, `START_IN_PROGRESS`, `DISPATCHER_START_FAILED`,
`THREAD_CREATE_FAILED`, `DISCOVERY_CREATE_FAILED`, `SCAN_NOT_FOUND`, `UNKNOWN_ERROR`,
`MALFORMED_REQUEST`, `UNKNOWN_ACTION`, `SERVER_BUSY`.

### `data/control_dispatcher_request_queue.c` / `.h`

Bounded FIFO of parsed `ControlRequest*`. Producer: lws thread (`_push`, one per
successfully-parsed inbound command). Consumer: worker thread (`_pop`, one per iteration of
its processing loop). Backed by a circular array of `capacity` owned-or-NULL pointers plus a
`Semaphore_create(1)`-as-mutex — **deliberately its own lock, separate from the ring
buffer's** (different producer/consumer thread pairs entirely; see §4).

- `ControlDispatcherRequestQueue_create(capacity)` — `capacity <= 0` → `NULL`.
- `ControlDispatcherRequestQueue_destroy` — frees the queue and every still-queued
  `ControlRequest` via `ControlDispatcherRequest_destroy`. NULL-safe.
- `ControlDispatcherRequestQueue_push(queue, request)` — never blocks. Returns `false` (queue
  still owns nothing, **caller retains ownership of `request`**) if at capacity — the lws
  thread treats this as `SERVER_BUSY` and must free `request` itself.
- `ControlDispatcherRequestQueue_pop(queue)` — never blocks. Returns `NULL` if empty
  (tolerated as a spurious wake by the worker's own loop). Caller owns the returned request.
- `ControlDispatcherRequest_destroy(request)` — frees every owned string field
  (`requestId`/`host`/`iedName`/`interfaceId`/`sclFilePath`/`acseAuthPassword`) plus the
  struct itself. NULL-safe.

### `data/control_dispatcher_ring_buffer.c` / `.h`

Bounded, thread-safe broadcast ring of pre-serialized JSON response strings — a near-verbatim
structural duplicate of `ipc_dispatcher_ring_buffer`/`scan_dispatcher_ring_buffer`
(deliberately duplicated, not shared — no precedent in this codebase for a cross-feature
"shared" directory). Producer here is this feature's **own worker thread**
(`control_dispatcher_worker.c`), not an external MMS/GOOSE producer — one push per finished
request. Reader: only the lws service-loop thread, once per connected client per writable
opportunity.

- `ControlDispatcherRingBuffer_create(capacity)` / `_destroy` — same shape as the request
  queue's own create/destroy.
- `ControlDispatcherRingBuffer_push(buffer, json)` — takes ownership of `json`
  unconditionally, O(1), never blocks. Overwrites the oldest slot on wraparound (freeing its
  previous string first).
- `ControlDispatcherRingBuffer_readNext(buffer, cursor, outDroppedForThisRead)` — advances
  `*cursor` by one, returns an owned copy of that slot's string (or `NULL` if `*cursor >=
  head`, i.e. caught up). If the cursor has fallen behind the oldest still-available message
  (`head - capacity`), it's snapped forward to `oldestAvailable` and the number of skipped
  messages is reported via `outDroppedForThisRead` — **a lagging client's cursor jumps
  forward and drops unseen messages; no backlog replay.**
- `ControlDispatcherRingBuffer_headSeq(buffer)` — current head sequence number, used to
  initialize a new connection's cursor (start-from-now semantics: no backlog on connect
  either).

### `data/control_dispatcher_worker.c` / `.h`

The dedicated thread doing the one genuinely slow thing in this feature: calling
`device_manager`'s `StartReporting`/`StopReporting` or `scan_orchestration`'s
`startScan`/`stopScan` (real network I/O / thread spin-up, seconds-scale), off the
libwebsockets service thread entirely.

`struct sControlDispatcherWorker` holds borrowed references to the request queue, ring
buffer, ws server, `DeviceManagerHandle`, and `ScanOrchestrationHandle`, plus its own
`Semaphore_create(0)` wake signal (posted once per queued request, plus once more on stop to
guarantee prompt exit even if idle — the same idiom `mms_report_client_connection.c`'s
supervisor thread already uses), `volatile bool stopRequested`/`exited`, and a `Thread`.

`workerLoop`: `Semaphore_wait(wake)` → check `stopRequested` → `pop` the request queue (a
`NULL` pop is a tolerated spurious wake or a racing stop) → **the slow part**:
`ControlDispatcherUseCases_processRequest` → `ControlDispatcherRequest_destroy` the popped
request → if a JSON response was produced, push it onto the ring buffer (ownership transfers)
and call `ControlDispatcherWsServer_wake` to have the lws thread write it out.

- `ControlDispatcherWorker_create` — allocates + creates the wake semaphore only, no thread
  yet. Rejects any `NULL` dependency with `CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT`.
- `ControlDispatcherWorker_start` — `Thread_create`/`Thread_start`. Error only on
  `Thread_create` failure.
- `ControlDispatcherWorker_notify` — posts the wake semaphore. This is the function wired as
  the ws server's `onRequestQueued` callback (via the `service/control_dispatcher_api.c`
  trampoline). Safe from the lws thread; never blocks.
- `ControlDispatcherWorker_stop` — sets `stopRequested`, posts the wake semaphore once more,
  then busy-polls (`Thread_sleep(20)`) until `exited` — `hal_thread.h` has no `Thread_join`,
  same idiom every other worker's stop function in this codebase already uses. Anything still
  queued at stop time is discarded, not drained. Must be called from the caller's own thread,
  never from within a request's own processing (deadlock). Safe to call more than once /
  before start.
- `ControlDispatcherWorker_destroy` — implies `_stop`, frees the thread handle and semaphore,
  frees the struct.

### `data/control_dispatcher_ws_server.c` / `.h` (304 lines)

Owns the `struct lws_context*` and the ONE thread that may ever touch it after creation —
adapted from `ipc_dispatcher_ws_server`/`scan_dispatcher_ws_server`, with one structural
addition those push-only transports don't need: `LWS_CALLBACK_RECEIVE` handling. Binds
`127.0.0.1` only, no TLS.

**`ControlDispatcherSession`** (per-connection `lws` user data, size given via
`per_session_data_size`):
```c
typedef struct {
    uint64_t cursor;                          /* read cursor into the ring buffer */
    char accumBuf[CONTROL_DISPATCHER_MAX_MESSAGE_SIZE]; /* 8192 bytes */
    size_t accumLen;
    bool overflowed;                          /* this message already exceeded the cap */
} ControlDispatcherSession;
```

`CONTROL_DISPATCHER_MAX_MESSAGE_SIZE` is `8192` — a safety cap against a runaway/malicious
sender, not a realistic legitimate control-message size.

`struct sControlDispatcherWsServer` holds the `lws_context*`, borrowed `ringBuffer`/
`requestQueue`, the `onRequestQueued` callback + its `void*` param, a two-element
`lws_protocols` array (`[1]` is the all-zero `LWS_PROTOCOL_LIST_TERM` terminator, satisfied
by `calloc`), `maxConnections`/`currentConnections`, and the usual `stopRequested`/
`serviceExited`/`serviceThread` triple.

**`controlDispatcherCallback`** (the one `lws_protocols` callback, `lws_context_user()`
retrieves the server struct back) handles, per `reason`:

- **`LWS_CALLBACK_ESTABLISHED`** — rejects the connection (`return -1`) if
  `currentConnections >= maxConnections`. Otherwise increments the count and initializes the
  session: `cursor = ControlDispatcherRingBuffer_headSeq(...)` (start-from-now, no backlog
  replay to a newly-connected client), `accumLen = 0`, `overflowed = false`.
- **`LWS_CALLBACK_CLOSED`** — decrements `currentConnections`.
- **`LWS_CALLBACK_RECEIVE`** — accumulates `len` bytes from `in` into `session->accumBuf`. If
  the accumulated length would exceed the 8KB cap, sets `overflowed` and silently drops
  further bytes of this message (still tracked, not memcpy'd). On `lws_is_final_fragment`:
  if `overflowed`, immediately writes a `MALFORMED_REQUEST` / "request exceeds maximum
  message size" error (no `requestId`/`action` recoverable — the buffer was never parsed);
  otherwise calls `handleCompleteMessage` on the full accumulated buffer. Resets
  `accumLen`/`overflowed` either way, ready for the next frame.
- **`LWS_CALLBACK_EVENT_WAIT_CANCELLED`** — fired by `lws_cancel_service`, woken by either
  the worker thread's `ControlDispatcherWsServer_wake` (a new response was pushed) or `_stop`
  (checked in the service loop itself, not here). Calls
  `lws_callback_on_writable_all_protocol` to give every connection a chance to drain the ring
  buffer.
- **`LWS_CALLBACK_SERVER_WRITEABLE`** — reads the next ring-buffer entry for this session's
  cursor via `ControlDispatcherRingBuffer_readNext` (the `dropped` count from a lagging
  cursor is computed but **not surfaced over the wire in v1**). If present, `lws_write`s it
  as a `LWS_WRITE_TEXT` frame (heap `malloc`'d with the required `LWS_PRE` header
  reservation, freed after write) and, if the ring buffer's head has moved further since
  this session's cursor advanced, calls `lws_callback_on_writable(wsi)` again immediately —
  drains the whole backlog for this connection without waiting for another external wake.

**`handleCompleteMessage(server, buf, len)`** — calls `ControlDispatcherJsonParser_parse`. On
any parse error, builds the error JSON directly via `control_dispatcher_json_writer`
(echoing back the best-effort recovered `requestId`/`action`) and pushes it straight onto the
ring buffer — **entirely on the lws thread**, never touching the worker or request queue. On
successful parse, tries `ControlDispatcherRequestQueue_push`; if the queue is full, destroys
the just-parsed request and pushes a `SERVER_BUSY` error — **also entirely on the lws
thread**. Only a successfully-queued request calls `server->onRequestQueued(...)` (the
worker's wake trampoline). Note: the `SERVER_BUSY` path's echoed `action` string is
hardcoded as a ternary between `"START_REPORTING"`/`"STOP_REPORTING"` only — a queue-full
`START_SCAN`/`STOP_SCAN` request's `SERVER_BUSY` response is mislabeled `"STOP_REPORTING"` in
its `action` field.

**`serviceLoop`** — `while (!stopRequested) lws_service(context, 1000)`. The 1000ms is a
bounded safety-net timeout for `lws_service`'s own blocking wait, not a data-driving poll —
real wakeups come from `lws_cancel_service` (new message OR stop request). Same narrow
"no cyclic polling" exception class as `ipc_dispatcher_ws_server.c`'s identical loop.

`ControlDispatcherWsServer_create` builds the `lws_context_creation_info` with
`info.iface = "127.0.0.1"` (hardcoded, not caller-configurable to anything else),
`info.gid`/`info.uid` set to `-1`, `info.user = server` (retrieved via `lws_context_user()`
in the callback). Returns `NULL` + `CONTROL_DISPATCHER_ERR_SOCKET_BIND_FAILED` if
`lws_create_context` fails.

`ControlDispatcherWsServer_wake` — the **one** libwebsockets call a producer thread (the
worker, after pushing a response) may make directly: `lws_cancel_service(context)`. Never
blocks.

`ControlDispatcherWsServer_stop`/`_destroy` — same shape as the worker's own stop/destroy:
set `stopRequested`, `lws_cancel_service` to force a prompt exit even if idle, busy-poll on
`serviceExited`, then (`_destroy` only) `Thread_destroy` + `lws_context_destroy` (closes the
listener and every connected client) + free.

---

## 4. Threading & concurrency model

Two threads created by `ControlDispatcher_start`, plus the caller's own thread for
`create`/`stop`/`destroy`:

1. **lws service thread** (`control_dispatcher_ws_server.c`'s `serviceLoop`) — the only
   thread that ever touches `struct lws_context*`. Runs `lws_service(context, 1000)` in a
   loop. Handles:
   - Accepting/closing connections, enforcing `maxConnections`.
   - Accumulating `LWS_CALLBACK_RECEIVE` fragments into the bounded 8KB per-session buffer,
     parsing the final fragment.
   - **A parse/validation failure or a full request queue is handled entirely on this
     thread** — builds and pushes the error JSON directly, no worker involvement.
   - A successfully parsed+queued request instead calls the `onRequestQueued`
     function-pointer callback (`ControlDispatcherWorker_notify`, wired at `_start` time) —
     this is a generic callback, not a direct dependency on the worker's type, so
     `control_dispatcher_ws_server.c` never needs to `#include` the worker header (the
     dependency runs the other way: the worker includes the ws-server header, for `_wake()`).
   - Draining the ring buffer per-connection on `LWS_CALLBACK_SERVER_WRITEABLE`, re-arming
     itself while backlog remains for that connection.
   - Waking on `lws_cancel_service`, called either by the worker thread (new response ready)
     or by `_stop` (exit request).

2. **request-worker thread** (`control_dispatcher_worker.c`'s `workerLoop`) — the dedicated
   thread that runs the one genuinely slow call in this feature:
   `ControlDispatcherUseCases_processRequest`, which reaches into `device_manager`'s
   `StartReporting`/`StopReporting` (real MMS/SCL network I/O, seconds-scale) or
   `scan_orchestration`'s `startScan`/`stopScan` (thread spin-up + first sweep dispatch).
   Blocks on a `Semaphore_create(0)` wake signal, posted once per successfully-queued
   request. After each request: pops the queue, processes it off the lws thread entirely,
   pushes the resulting JSON onto the ring buffer, then calls
   `ControlDispatcherWsServer_wake` — the **only** libwebsockets call this producer thread
   makes directly (`lws_cancel_service`, safe from any thread).

3. **Two independent locks, deliberately not shared:**
   - The **request queue**'s own `Semaphore_create(1)` mutex — producer: lws thread
     (`_push`), consumer: worker thread (`_pop`).
   - The **ring buffer**'s own `Semaphore_create(1)` mutex — producer: worker thread
     (`_push`, one response per finished request), consumer: lws thread (`_readNext`, once
     per connection per writable opportunity).
   These guard two structurally independent producer/consumer relationships running in
   opposite directions between the same two threads; reusing one lock for both would be safe
   but would couple them for no benefit (documented explicitly in
   `control_dispatcher_request_queue.h`'s header comment).

4. **Fan-out is broadcast to every connected control client** — the ring buffer has no
   per-request routing; every response (success or error, for every action) is written to
   every currently-connected session as it drains its own cursor. A client only recognizes
   "its" response by matching `requestId` client-side (see the E2E test's `waitForResponse`,
   which loops reading frames until one matches the expected `requestId`).

5. **No unsolicited push exists** — the only two threads that ever push onto the ring buffer
   are the lws thread itself (synchronous parse/queue-full errors) and the worker thread
   (finished request results). There is no third path (e.g. a device-health monitor) that
   pushes an unprompted message; every message on the wire is a response to some prior
   inbound request.

---

## 5. Known limitations / deliberate scope boundaries

- **Loopback-only, one-real-client trust assumption.** Bind is hardcoded to `127.0.0.1`, no
  TLS, no auth on the control channel itself (`START_SCAN`/`STOP_SCAN` don't even accept an
  `acseAuthPassword` field — see the JSON envelope). Broadcast fan-out (§4) is a real tradeoff
  if this "one real, trusted, local client" assumption ever changes — any connected client
  sees every other client's request/response traffic.
- **No unsolicited `DEVICE_STOPPED` (or similar) push.** A device only leaves
  `device_manager`'s registry via an explicit `STOP_REPORTING` round trip. `device_manager`
  itself does not watch connection health or auto-stop a device on connection loss (see its
  own Architecture bullet) — so there is structurally nothing for this feature to push
  unsolicited even if it wanted to.
- **`SERVER_BUSY`'s echoed `action` field is wrong for scan requests** — `handleCompleteMessage`
  in `control_dispatcher_ws_server.c` hardcodes the echoed action as a
  `START_REPORTING`/`STOP_REPORTING` ternary; a `START_SCAN`/`STOP_SCAN` request that hits a
  full request queue gets a `SERVER_BUSY` error with `action: "STOP_REPORTING"` regardless of
  its real action.
- **Dropped ring-buffer messages for a lagging client are computed but not surfaced** —
  `ControlDispatcherRingBuffer_readNext`'s `outDroppedForThisRead` is silently discarded in
  `LWS_CALLBACK_SERVER_WRITEABLE` ("not surfaced over the wire in v1" per the source comment).
  A sufficiently slow/lagging client can silently skip responses to its own earlier requests
  if 256+ other responses land in between.
- **8KB inbound message cap** is a hardcoded `#define`, not part of `ControlDispatcherConfig`.
- **stop() has no graceful drain** — both `ControlDispatcherWorker_stop` and
  `ControlDispatcherWsServer_stop` discard anything still queued/buffered rather than
  finishing it, consistent with `IpcDispatcher_stop`/`ScanDispatcher_stop`'s own behavior
  elsewhere in this codebase.

---

## 6. Cross-feature dependencies

`control_dispatcher` depends directly on both of the daemon's top-level orchestration
registries — not on any `src/features/` peer:

- **`device_manager`** (`src/device_manager/`) — `DeviceManager_startReporting`/
  `_stopReporting`, relayed from `START_REPORTING`/`STOP_REPORTING`. Runs several
  `orchestration` pipelines concurrently, one per physical IED, each auto-assigned its own
  `ipc_dispatcher` port, addressable by a server-generated `deviceId`.
- **`scan_orchestration`** (`src/scan_orchestration/`) — `ScanOrchestration_startScan`/
  `_stopScan`, relayed from `START_SCAN`/`STOP_SCAN`. Sequences `ied_discovery` and
  `scan_dispatcher` into a continuous, background, reference-counted, multi-scan-capable
  service.

Both `device_manager` and `scan_orchestration` are top-level siblings of `src/features/`
(like `src/orchestration/`) — not `src/features/` peers themselves. Depending on both is the
entire point of this feature (it's the control-plane relay for exactly these two
registries), not a layering violation. Both handles are **borrowed** at `ControlDispatcher_create`
time; `control_dispatcher` never creates, destroys, or otherwise owns either. `main.c`
creates `device_manager` + `scan_orchestration` + `control_dispatcher` in that order, and
tears them down in reverse order — `control_dispatcher`'s own lifecycle is owned entirely by
`main.c` directly, unlike `ipc_dispatcher` (owned by `orchestration`, per-device) or
`scan_dispatcher` (owned by `scan_orchestration`, refcounted by active-scan count).

---

## 7. Tests

### `tests/control_dispatcher/` (unit, Unity — see `tests/Makefile`'s `TESTS` list)

- **`test_control_dispatcher_ring_buffer.c`** — invalid capacity, `headSeq` starts at zero,
  read-at-head returns `NULL`, push-then-read returns an owned copy (not an alias),
  wraparound drops the oldest entries, two independent cursors (one lagging) don't interfere
  with each other, push takes ownership even when the buffer pointer itself is `NULL`,
  destroy is NULL-safe.
- **`test_control_dispatcher_request_queue.c`** — invalid capacity, pop-when-empty, FIFO
  push/pop ordering, push-when-full returns `false` with the caller retaining ownership,
  wraparound after a pop-then-push cycle, destroy frees every still-queued request, general
  NULL-safety.
- **`test_control_dispatcher_json_writer.c`** — shape assertions for
  `writeStartSuccess`/`writeStopSuccess`/`writeScanStartSuccess`/`writeScanStopSuccess`,
  `writeError` with and without `stage`/`detail`, `writeError` with `NULL` `requestId`/
  `action` producing JSON `null`.
- **`test_control_dispatcher_json_parser.c`** — the largest suite (22 tests): malformed JSON,
  non-object root, missing/empty `requestId` (with action-recovery check), unknown/missing
  `action` (with requestId+action recovery check), missing `params`, and per-action
  positive/negative param validation for all four action types (missing required fields,
  `sclFilePath`-without-`iedName`, unknown `accessMode`, minimal-valid defaults, fully
  populated, negative `deviceId`/`scanId`).
- **`test_control_dispatcher_usecases.c`** — dispatch/error-mapping only, driven against a
  REAL `DeviceManagerHandle` and `ScanOrchestrationHandle` in deliberately-failing, no-network
  cases (empty host → `INVALID_ARGUMENT`, unknown `deviceId` → `DEVICE_NOT_FOUND`, `NULL`
  `interfaceId` → `INVALID_ARGUMENT` for `START_SCAN`, unknown `scanId` →
  `SCAN_NOT_FOUND`). The success-path JSON mapping (a real `StartReporting`/`StartScan`
  succeeding) is proven only by the E2E test below, which has a real IED/interface to talk
  to.
- **`test_control_dispatcher_api.c`** — real bind on loopback (dedicated high port range
  `18767`+, to avoid clashing with a real daemon instance on the default 8767): config
  defaults match documented values, `NULL`-config applies defaults, rejects `NULL`
  `deviceManager`/`scanOrchestration`, rejects invalid capacities, stop-before-start is a
  no-op, double-start returns `ALREADY_RUNNING`, start→stop→start cleanly rebinds the same
  port (no `EADDRINUSE`), NULL-safety on `_stop`/`_destroy`/`_start`. Never sends a real
  websocket frame at this layer — that's the E2E test's job.

### `integration_tests/control_dispatcher/e2e_test_control_dispatcher.c`

Real bind of `control_dispatcher` on a test port, driven by a hand-rolled minimal RFC6455
client — **the first E2E test in this repo to send a client→server frame** (every other
dispatcher is push-only). Implements masked client→server text frames (required by RFC6455,
which no prior push-only-transport test needed) alongside the usual unmasked server→client
frame reader.

- **`test_malformedJson_returnsMalformedRequestError`** — sends `"{not valid json"`, asserts
  a `MALFORMED_REQUEST` error response. No network I/O — fails entirely on the lws thread
  before ever reaching `device_manager`.
- **`test_unknownAction_returnsUnknownActionError`** — sends a well-formed envelope with
  `action: "DO_A_BARREL_ROLL"`, asserts `UNKNOWN_ACTION`.
- **`test_startReporting_thenStopReporting_realRoundTrip`** — real `START_REPORTING` against
  a real `ied_simulator` "Reporter1" instance, asserting the returned `wsPort` streams real
  GOOSE JSON (connects to the per-device `ipc_dispatcher` port and waits for a
  `gocbRef`-matching `GOOSE` message after flipping the simulator's indication twice — the
  first flip is a throwaway seed against `goose_subscriber`'s own bootstrap-suppression of
  the first-ever frame per target), proving the full chain: control message → `device_manager`
  → `orchestration` → per-device `ipc_dispatcher`. Then a real `STOP_REPORTING`, and confirms
  the per-device port is actually torn down afterward (a follow-up connect attempt must
  fail). **Needs `sudo`/`CAP_NET_RAW`** (inherited transitively from the GOOSE subscriber step
  every `DeviceManager_startReporting` call reaches via orchestration).
- **`test_startScan_thenStopScan_realRoundTrip`** — real `START_SCAN`/`STOP_SCAN` over `lo`.
  No `sudo` needed (a scan sweep is TCP-only). `lo`'s sweep is expected to fail in the
  background (netmask too large for `maxHosts`) but that's tolerated gracefully by the scan
  worker thread, not a synchronous failure of `START_SCAN` itself — this proves the
  control-plane round trip, not sweep success (same reasoning
  `integration_tests/scan_orchestration/`'s own E2E test documents).

Run: `cd integration_tests/control_dispatcher && sudo make run` (the malformed-JSON/
unknown-action/scan cases don't need the privilege or the simulator, but the suite runs as
one binary).
