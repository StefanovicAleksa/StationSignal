# scan_dispatcher

## 1. Overview

`scan_dispatcher` relays "device found" scan events over its own loopback-only websocket
(default port **8766**) to whatever client is watching a subnet scan in progress. It exists
because `scan_orchestration` needs to stream discovery results out of the daemon the same way
`ipc_dispatcher` streams MMS/GOOSE reports out — but scan results are a structurally different,
much simpler stream (one event type, no quality/value/reference machinery), so they get their own
transport rather than being shoehorned into `ipc_dispatcher`'s envelope.

It has **no knowledge of scans, interfaces, or reference-counting** — purely a transport. It
doesn't know how many scans are active, doesn't dedup discovered hosts, and doesn't decide when
it should be running. `scan_orchestration` owns all of that: it decides when to call
`ScanDispatcher_start`/`_stop` (refcounted by active-scan count — started on the first
concurrently-active scan, 0→1; stopped at 1→0) and is the only caller of
`ScanDispatcher_publishDeviceFound`. One shared `ScanDispatcher` instance serves every
concurrently-running scan; events from different scans are distinguished only by the `scanId`
field in the JSON payload.

Public API header: `src/features/scan_dispatcher/service/scan_dispatcher_api.h`.

## 2. Public API surface

All declared in `src/features/scan_dispatcher/service/scan_dispatcher_api.h`:

- **`void ScanDispatcherConfig_defaults(ScanDispatcherConfig* config)`** — fills `config` with
  `port=8766`, `ringBufferCapacity=256`, `maxConnections=16`. NULL-safe (no-op on NULL). Caller
  may override individual fields before passing to `ScanDispatcher_create`.
- **`ScanDispatcherHandle ScanDispatcher_create(const ScanDispatcherConfig* config,
  ScanDispatcherError* outError)`** — allocates the handle and the ring buffer only; no
  socket/thread yet (matches every other feature's "no I/O at create" contract).
  `config == NULL` means apply defaults. Returns `NULL` and sets `*outError` to
  `SCAN_DISPATCHER_ERR_OUT_OF_MEMORY` or `SCAN_DISPATCHER_ERR_INVALID_ARGUMENT` (non-positive
  `ringBufferCapacity`/`maxConnections`) on failure.
- **`ScanDispatcherError ScanDispatcher_start(ScanDispatcherHandle handle)`** — binds
  `127.0.0.1:config.port` and starts the dedicated libwebsockets service-loop thread.
  Non-blocking once the synchronous bind succeeds. Returns
  `SCAN_DISPATCHER_ERR_ALREADY_RUNNING` if called twice without an intervening `_stop()`,
  `SCAN_DISPATCHER_ERR_SOCKET_BIND_FAILED` if the port is unavailable, or
  `SCAN_DISPATCHER_ERR_INVALID_ARGUMENT` on a NULL handle.
- **`void ScanDispatcher_stop(ScanDispatcherHandle handle)`** — stops the service-loop thread and
  closes the listening socket plus every connected client. Blocking. Must be called from the
  caller's own thread, never from inside a callback (deadlock). Safe to call more than once / on
  a never-started handle (no-op). Fully tears down the ws server (thread + lws context), not just
  the thread, so a subsequent `ScanDispatcher_start` can rebind the port cleanly.
- **`void ScanDispatcher_destroy(ScanDispatcherHandle handle)`** — implies `_stop()` if still
  running, then frees the handle including the ring buffer. NULL-safe.
- **`void ScanDispatcher_publishDeviceFound(ScanDispatcherHandle handle, uint64_t scanId, const
  char* host, int mmsPort)`** — non-blocking: serializes the event to JSON, enqueues it onto the
  ring buffer, and wakes the service thread. Safe to call from any scan worker thread
  concurrently (multiple scans' worker threads may call this on the shared instance at once).
  No-op if `handle` is NULL or the dispatcher isn't currently running.

## 3. Per-file breakdown

### `service/scan_dispatcher_api.h` / `.c`
Public boundary and thin orchestration layer. `scan_dispatcher_api.c` wires the three internal
collaborators together: `ScanDispatcherRingBuffer` (created in `_create`, destroyed in
`_destroy`), `ScanDispatcherWsServer` (created/started in `_start`, destroyed in `_stop`), and
`ScanDispatcherAdapter` (the real logic behind `_publishDeviceFound`, called via a one-line
delegation). Holds no business logic itself — matches `ipc_dispatcher_api.c`'s own shape.

### `domain/scan_dispatcher_types.h`
Domain vocabulary, zero third-party includes. Defines:
- `ScanDispatcherError` enum: `SCAN_DISPATCHER_OK`, `_ERR_INVALID_ARGUMENT`, `_ERR_OUT_OF_MEMORY`,
  `_ERR_THREAD_CREATE_FAILED`, `_ERR_SOCKET_BIND_FAILED`, `_ERR_ALREADY_RUNNING`.
- `ScanDispatcherConfig` struct: `port` (uint16_t, default 8766 — deliberately distinct from
  `ipc_dispatcher`'s 8765 since both can be bound in the same process), `ringBufferCapacity` (int,
  default 256), `maxConnections` (int, default 16).
- `ScanDeviceFoundEvent` struct: `scanId` (uint64_t), `host` (owned `char*` copy), `mmsPort`
  (int), `discoveredAtMs` (uint64_t) — this is the JSON contract's own vocabulary, not borrowed
  from `ied_discovery`/`scan_orchestration`.
- Opaque forward declarations (`struct sScanDispatcherRingBuffer`, `struct sScanDispatcherWsServer`)
  kept as bare tags so this header never pulls in `hal_thread.h`/`libwebsockets.h`.
- `struct sScanDispatcherHandle` (the concrete backing of the opaque `ScanDispatcherHandle`):
  `config`, owned `ringBuffer` pointer (created in `ScanDispatcher_create`), owned `wsServer`
  pointer (NULL when not running, created/destroyed in `_start`/`_stop`), `volatile bool running`.

Top-of-file comment documents the deliberate-duplication rationale (see section 6).

### `domain/scan_dispatcher_usecases.h` / `.c`
Pure logic, zero third-party includes, unit-testable directly.
- **`ScanDeviceFoundEvent* ScanDispatcherUseCases_assembleEvent(uint64_t scanId, const char*
  host, int mmsPort, uint64_t discoveredAtMs)`** — deep-copies `host` (via `strdup`) into a newly
  allocated `ScanDeviceFoundEvent`. Returns `NULL` on allocation failure or a NULL/empty host.
  Caller owns the result.
- **`void ScanDispatcherUseCases_freeEvent(ScanDeviceFoundEvent* event)`** — frees the event and
  its owned `host` string. NULL-safe.

### `data/scan_dispatcher_adapter.h` / `.c`
The real implementation behind `ScanDispatcher_publishDeviceFound` (mirrors
`ipc_dispatcher_api.c`'s own delegation into `data/ipc_dispatcher_mms_adapter.c`).
`ScanDispatcherAdapter_publishDeviceFound`: no-op if `handle` is NULL or not running; otherwise
assembles the event via `ScanDispatcherUseCases_assembleEvent` (timestamped with
`Hal_getTimeInMs()`), serializes it via `ScanDispatcherJsonWriter_write`, frees the event, and if
serialization succeeded, pushes the JSON string onto the ring buffer (`ScanDispatcherRingBuffer_push`
— ownership transfers to the ring buffer) and wakes the service thread
(`ScanDispatcherWsServer_wake`). Fast, non-blocking, never touches a `struct lws*` directly — safe
to call from any scan's own worker thread.

### `data/scan_dispatcher_json_writer.h` / `.c`
The only file in this feature that includes `cJSON.h`. One function:
**`char* ScanDispatcherJsonWriter_write(const ScanDeviceFoundEvent* event)`** — returns a
heap-allocated, NUL-terminated JSON string via `cJSON_PrintUnformatted` (no pretty-printing —
this is a wire payload), or `NULL` on a NULL event or allocation failure. Caller owns the
returned string (`free()`). No parse counterpart exists anywhere in this feature — push-only, no
client-sent message handling.

**JSON envelope** (stable contract):
```json
{
  "schemaVersion": 1,
  "type": "SCAN_RESULT",
  "scanId": 1,
  "host": "192.168.1.50",
  "mmsPort": 102,
  "discoveredAtMs": 1751520000123
}
```
Known caveat: cJSON numbers are double-backed, so `scanId`/`discoveredAtMs` beyond 2^53 lose
precision — harmless at these magnitudes (a monotonic per-process scan-id counter, a wall-clock
ms timestamp).

### `data/scan_dispatcher_ring_buffer.h` / `.c`
Bounded, thread-safe broadcast ring of pre-serialized JSON strings (fixed capacity, no dynamic
growth). Near-verbatim port of `ipc_dispatcher_ring_buffer.{h,c}`. Opaque type
`ScanDispatcherRingBuffer` backed by `struct sScanDispatcherRingBuffer { char** slots; int
capacity; uint64_t head; Semaphore lock; }`. `lock` is a `hal_thread.h` `Semaphore` used as a
binary mutex (`Semaphore_create(1)`) — `hal_thread.h` has no separate `Mutex` type.

- `ScanDispatcherRingBuffer_create(int capacity)` — `NULL` on non-positive capacity or allocation
  failure.
- `ScanDispatcherRingBuffer_destroy(buffer)` — frees the ring and every still-buffered string it
  owns. NULL-safe.
- `ScanDispatcherRingBuffer_push(buffer, char* json)` — producer side. Takes ownership of `json`
  unconditionally, even on a NULL buffer (just frees it). Never blocks, O(1): stores at
  `slots[head % capacity]`, frees whatever was previously in that slot (implicit drop-oldest),
  advances `head`.
- `ScanDispatcherRingBuffer_readNext(buffer, uint64_t* cursor, uint64_t* outDroppedForThisRead)` —
  reader side. If `*cursor == head`: nothing new, returns `NULL`. If the reader's next-unread
  message was already overwritten (`head - *cursor > capacity`): fast-forwards `*cursor` to
  `head - capacity` and reports the skip count via `*outDroppedForThisRead`. Otherwise returns a
  freshly heap-allocated **copy** of the message at `*cursor` and advances `*cursor` by 1. Caller
  owns the returned string.
- `ScanDispatcherRingBuffer_headSeq(buffer)` — current head sequence number, used to initialize a
  new connection's cursor (start-from-now semantics, no backlog replay).

Producer side is called from every active scan's own worker thread (`scan_orchestration`'s
per-scan sweep loop) — potentially multiple concurrent producers. Reader side is called only from
this feature's own libwebsockets service-loop thread, once per connected client per writable
opportunity.

### `data/scan_dispatcher_ws_server.h` / `.c` (198 lines)
Owns the libwebsockets `struct lws_context*` and the one dedicated thread that may ever touch it
after creation. Near-verbatim port of `ipc_dispatcher_ws_server.{h,c}`. Binds `127.0.0.1` only, no
TLS, not caller-configurable to any other interface. Opaque type `ScanDispatcherWsServer` backed
by:
```c
struct sScanDispatcherWsServer {
    struct lws_context* context;
    ScanDispatcherRingBuffer ringBuffer;   /* borrowed */
    struct lws_protocols protocols[2];     /* [1] is the all-zero LWS_PROTOCOL_LIST_TERM terminator */
    int maxConnections;
    int currentConnections;
    volatile bool stopRequested;
    volatile bool serviceExited;
    Thread serviceThread;
};
```
Per-connection session data (`ScanDispatcherSession`) is just a `uint64_t cursor` into the shared
ring buffer, stored via lws's `per_session_data_size` mechanism.

**`scanDispatcherCallback`** (the one `struct lws_protocols` callback, protocol name
`"scan-dispatcher-v1"`) handles:
- `LWS_CALLBACK_ESTABLISHED` — rejects (`return -1`) if `currentConnections >= maxConnections`;
  otherwise increments the connection count and seeds the new session's cursor to
  `ScanDispatcherRingBuffer_headSeq` (start-from-now — no backlog replay for a newly connected
  client).
- `LWS_CALLBACK_CLOSED` — decrements `currentConnections`.
- `LWS_CALLBACK_EVENT_WAIT_CANCELLED` — fired by either a scan worker's
  `ScanDispatcherWsServer_wake` (new message pushed) or `_stop`'s wakeup (exit request, checked
  separately in the service loop itself). Calls `lws_callback_on_writable_all_protocol` to give
  every connection a chance to drain.
- `LWS_CALLBACK_SERVER_WRITEABLE` — drains one message via `ScanDispatcherRingBuffer_readNext`,
  writes it as an `LWS_WRITE_TEXT` frame (buffer allocated with the required `LWS_PRE` headroom).
  The `dropped` count (lagging-client messages skipped) is computed but **not surfaced over the
  wire in v1**. If more is still buffered for this connection after the write
  (`headSeq != session->cursor`), re-arms `lws_callback_on_writable(wsi)` immediately rather than
  waiting for another external wake — drains fully before going idle again.

**`serviceLoop`** — the dedicated thread body: loops `lws_service(context, 1000)` until
`stopRequested`. The 1000ms figure is a bounded safety-net timeout for lws's own blocking wait,
not a data-driving poll — real wakeups come from `lws_cancel_service` (new message or stop
request). Same narrow "no cyclic polling" exception class as `ipc_dispatcher_ws_server.c`'s
identical loop (see CLAUDE.md's Hard Rules).

Lifecycle functions:
- `ScanDispatcherWsServer_create(port, maxConnections, ringBuffer, outError)` — performs the
  actual bind (`lws_create_context` with `info.iface="127.0.0.1"`, `info.port=port`), deliberately
  not done at `ScanDispatcher_create` time. `ringBuffer` is borrowed. Returns `NULL` +
  `SCAN_DISPATCHER_ERR_SOCKET_BIND_FAILED` on bind failure (e.g. port in use), or +
  `SCAN_DISPATCHER_ERR_INVALID_ARGUMENT` on a NULL ring buffer / non-positive `maxConnections`.
- `ScanDispatcherWsServer_start(server)` — starts the service-loop thread
  (`Thread_create`/`Thread_start`). Non-blocking. Returns `SCAN_DISPATCHER_ERR_THREAD_CREATE_FAILED`
  only on thread-creation failure.
- `ScanDispatcherWsServer_wake(server)` — calls `lws_cancel_service(context)`. The **one**
  libwebsockets call a producer thread may make directly. Called immediately after a successful
  `ScanDispatcherRingBuffer_push`. Never blocks.
- `ScanDispatcherWsServer_stop(server)` — sets `stopRequested`, wakes via `lws_cancel_service` for
  prompt exit even if idle, then busy-waits (`Thread_sleep(20)` polling `serviceExited`) until the
  service thread has actually exited. Must be called from the caller's own thread, never from
  inside an lws callback (deadlock). Safe to call more than once / before start.
- `ScanDispatcherWsServer_destroy(server)` — implies `_stop()`, then `Thread_destroy`s the thread
  handle and `lws_context_destroy`s the context (closing the listener and every connected client),
  frees the struct. NULL-safe.

## 4. Threading & concurrency model

Two thread roles, matching `ipc_dispatcher`'s design exactly:

- **Producer threads** — one or more `scan_orchestration` per-scan worker threads. Each calls
  `ScanDispatcher_publishDeviceFound` → `ScanDispatcherAdapter_publishDeviceFound`, which
  serializes to JSON, pushes onto the ring buffer (mutex-guarded, O(1), never blocks), and calls
  `ScanDispatcherWsServer_wake` (`lws_cancel_service`) — the only libwebsockets call a producer
  thread is allowed to make directly. Producers never touch `struct lws*` or the context.
- **The service-loop thread** — the single thread created by `ScanDispatcherWsServer_start`,
  the only thread that ever calls into libwebsockets proper (`lws_service`, `lws_write`,
  connection accept/close). It drains the ring buffer per-connection via each session's own read
  cursor on `LWS_CALLBACK_SERVER_WRITEABLE`, triggered either by a producer's wake or its own
  1000ms safety-net timeout.
- A lagging client's cursor is fast-forwarded past overwritten slots — no backlog replay, dropped
  count is computed internally but not sent to the client.
- `ScanDispatcher_stop`/`ScanDispatcherWsServer_stop` must be called from a thread other than the
  service-loop thread itself (i.e. never from inside `scanDispatcherCallback`) — calling it from
  within a callback would deadlock waiting for `serviceExited` on the same thread that has to set
  it.
- The ring buffer's mutex (a `hal_thread.h` `Semaphore` used as a binary lock) is a separate lock
  from anything in `scan_orchestration` or `ied_discovery` — this feature has no shared state with
  its callers beyond the borrowed ring buffer pointer.

## 5. Known limitations / deliberate scope boundaries

- **No knowledge of scans, interfaces, or refcounting** — purely transport. Doesn't dedup
  discovered hosts (that's `scan_orchestration`'s per-scan seen-set), doesn't know how many scans
  are active, doesn't decide its own start/stop timing.
- **Push-only** — no client-sent message handling of any kind (no parse counterpart to the JSON
  writer exists).
- **Fan-out is broadcast to every connected client** on the shared instance — same trust
  assumption as `ipc_dispatcher`/`control_dispatcher` (loopback-only, trusted local clients).
  There is no per-`scanId` subscription filtering; a client watching one scan receives every
  concurrently-running scan's events and must filter by `scanId` itself.
  - Dropped-message count for a lagging client is computed (`outDroppedForThisRead`) but not
  surfaced over the wire in v1 — a lagging client only sees a gap, no explicit signal.
- Start-from-now semantics only — a client connecting after a device was already found gets no
  backlog replay of that event.
- No TLS, binds `127.0.0.1` only, not configurable to any other interface — internal-only by
  design.
- `ScanDispatcherWsServer_stop`'s wait for `serviceExited` is a 20ms busy-poll loop, not a
  condition variable — bounded but not instantaneous.

## 6. Cross-feature dependencies

- **Owned entirely by `scan_orchestration`**, refcounted by active-scan count: started on the
  0→1 transition (first concurrently-active scan), stopped on the 1→0 transition (last scan
  stops). `scan_dispatcher` itself has no notion of this refcounting — it's implemented entirely
  in `src/scan_orchestration/`.
- **Deliberately duplicated from `ipc_dispatcher`, not shared.** `scan_dispatcher_types.h`'s
  top-of-file comment states the rationale explicitly: this feature is a near-verbatim structural
  duplicate of `ipc_dispatcher`'s ring-buffer + libwebsockets-service-thread transport
  (`scan_dispatcher_ring_buffer.{h,c}` mirrors `ipc_dispatcher_ring_buffer.{h,c}`;
  `scan_dispatcher_ws_server.{h,c}` mirrors `ipc_dispatcher_ws_server.{h,c}`), but there is no
  precedent anywhere in this codebase for a cross-feature "shared" directory — the existing
  convention is to duplicate small third-party-integration code per feature (ACSE-auth setup is
  already duplicated 4x elsewhere). The two dispatchers differ only in: default port (8766 vs
  8765 — both can be bound in the same process, since `main.c`'s scan flow runs before
  `Orchestration_run` starts `ipc_dispatcher`), protocol name (`"scan-dispatcher-v1"` vs
  `ipc_dispatcher`'s equivalent), JSON envelope shape/contents, and the number of producer-side
  adapters (`scan_dispatcher` has exactly one — `publishDeviceFound`; `ipc_dispatcher` has two,
  one each for MMS reports and GOOSE records).
- No dependency on `ied_discovery` or `ied_model` — those live entirely upstream in
  `scan_orchestration`'s own sequencing.

## 7. Tests

**`tests/scan_dispatcher/`** (unit tests, Unity framework, wired into `tests/Makefile`'s `TESTS`
list — no auto-discovery):
- `test_scan_dispatcher_usecases.c` — `ScanDispatcherUseCases_assembleEvent`/`_freeEvent`: deep
  host copy (not aliased), NULL/empty-host rejection, NULL-safe free.
- `test_scan_dispatcher_json_writer.c` — `ScanDispatcherJsonWriter_write` round-trips every field
  through `cJSON_Parse` (schemaVersion, type, scanId, host, mmsPort, discoveredAtMs), and returns
  `NULL` on a NULL event.
- `test_scan_dispatcher_ring_buffer.c` — invalid-capacity rejection, `headSeq` starts at 0,
  `readNext` returns `NULL` at head, push-then-read returns a copy (not an alias) with
  `dropped==0`, wraparound correctly reports the dropped count and skips to the oldest still
  available message, `push` always takes ownership even on a NULL buffer, NULL-safe destroy. A
  near-verbatim duplicate of `test_ipc_dispatcher_ring_buffer.c` against the duplicated
  implementation.
- `test_scan_dispatcher_api.c` — config defaults match documented values (port 8766, capacity
  256, maxConnections 16), NULL-safe `_defaults`, `_create` applies defaults when config is NULL
  and rejects invalid capacities, `_stop` before `_start` is a no-op, double `_start` returns
  `SCAN_DISPATCHER_ERR_ALREADY_RUNNING`, start→stop→start on the same port rebinds cleanly (no
  `EADDRINUSE`), NULL-safety on `_stop`/`_destroy`/`_start`/`_publishDeviceFound`, and
  `_publishDeviceFound` is a no-op when not running. Real bind on a dedicated high port range
  (18766+) to avoid clashing with a real daemon instance on the default 8766.

**`integration_tests/scan_dispatcher/`** (E2E, no `sudo` needed — plain TCP/loopback, no real
GOOSE):
- `e2e_test_scan_dispatcher.c` — starts a real `ScanDispatcher` (real bind, real libwebsockets
  service thread), connects a hand-rolled minimal RFC6455 websocket test client (raw TCP +
  HTTP-Upgrade handshake + a small unmasked/unfragmented text-frame parser — deliberately not
  using libwebsockets client mode, so a bug shared by both client and server library code
  couldn't be masked), and asserts:
  - `test_deviceFound_arrivesAsScanResultJson` — one `ScanDispatcher_publishDeviceFound` call
    arrives over the real socket as a `SCAN_RESULT` JSON envelope with all expected fields,
    parsed back via cJSON (not raw string equality).
  - `test_multipleDeviceFound_arriveInOrder` — two publishes arrive as two frames in the same
    order they were published.
  - Sec-WebSocket-Accept is intentionally not verified (would need hand-rolled SHA1 for no
    benefit to the test's actual goal) — the "101" status line is enough to confirm the upgrade
    succeeded.
  - A near-verbatim duplicate of `integration_tests/ipc_dispatcher/e2e_test_ipc_dispatcher.c`'s
    own test-client helpers.
- `Makefile` builds this suite standalone; wired into `run_all_tests.sh` as
  `run_suite "e2e: scan_dispatcher" "integration_tests/scan_dispatcher"`.
