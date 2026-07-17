# `ipc_dispatcher`

## 1. Overview

`ipc_dispatcher` relays normalized IEC 61850 data out of the daemon: it takes
`MmsReportRecord`s (from `mms_report_client`) and `GooseSubscriberRecord`s
(from `goose_subscriber`), converts each into a stable JSON envelope, and
pushes it to every connected client over a loopback-only websocket. It is the
daemon's one "reporting" surface — the boundary between internal C structs
(which carry raw `MmsValue*`/GOOSE-frame data) and the external API
layer/frontend, which only ever sees JSON. It is push-only: there is no
inbound message handling, no request/response, nothing to parse from a
client.

Public boundary: `src/features/ipc_dispatcher/service/ipc_dispatcher_api.h`.
Its two callback-adapters (`IpcDispatcher_onMmsReport`,
`IpcDispatcher_onGooseRecord`) are registered directly and unconditionally by
`src/orchestration/` — `Orchestration_setReportCallback`/
`Orchestration_setGooseRecordCallback` are called once per `orchestration`
run, pointed at the one `IpcDispatcherHandle` created for that IED. There is
no mechanism to register a second consumer; each delivered record's ownership
transfers entirely to `ipc_dispatcher`, which is responsible for destroying
it (`MmsReportClient_destroyReportRecord`/`GooseSubscription_destroyRecord`)
before its callback returns. Single-IED scope, matching the whole daemon's
per-device architecture (`device_manager` runs one `ipc_dispatcher` instance
per registered device, each bound to its own auto-assigned port).

## 2. Public API surface

All from `src/features/ipc_dispatcher/service/ipc_dispatcher_api.h`:

- **`void IpcDispatcherConfig_defaults(IpcDispatcherConfig* config)`** —
  fills `port=8765`, `ringBufferCapacity=256`, `maxConnections=16`. Caller may
  override any field before passing to `_create`.
- **`IpcDispatcherHandle IpcDispatcher_create(const IpcDispatcherConfig* config, IpcDispatcherError* outError)`**
  — allocation only, no socket/thread yet (creates the ring buffer, which is
  pure allocation, no I/O) — matches `MmsReportClient_create`/
  `GooseSubscription_create`'s own "no I/O at create" contract. `config ==
  NULL` means defaults. Returns `NULL` + sets `*outError` only on
  argument/allocation failure (`IPC_DISPATCHER_ERR_INVALID_ARGUMENT` if
  `ringBufferCapacity <= 0 || maxConnections <= 0`; `IPC_DISPATCHER_ERR_OUT_OF_MEMORY`
  on allocation failure).
- **`IpcDispatcherError IpcDispatcher_start(IpcDispatcherHandle handle)`** —
  binds `127.0.0.1:config.port` and starts the dedicated libwebsockets
  service-loop thread. Non-blocking once the synchronous bind succeeds.
  Returns `IPC_DISPATCHER_ERR_ALREADY_RUNNING` if called twice without an
  intervening `_stop()`; `IPC_DISPATCHER_ERR_SOCKET_BIND_FAILED` if the port
  is unavailable; `IPC_DISPATCHER_ERR_INVALID_ARGUMENT` on a `NULL` handle.
- **`void IpcDispatcher_stop(IpcDispatcherHandle handle)`** — stops the
  service-loop thread (bounded, prompt — woken via `lws_cancel_service`) and
  closes the listening socket plus every connected client. Blocking. MUST be
  called from the caller's own thread, never from within either callback
  below (deadlock — same rule as every other feature's `_stop()`). Safe to
  call more than once / on a never-started handle (no-op).
- **`void IpcDispatcher_destroy(IpcDispatcherHandle handle)`** — implies
  `IpcDispatcher_stop()` if still running, then frees the handle including the
  ring buffer.
- **`void IpcDispatcher_onMmsReport(void* userParam, const MmsReportRecord* record)`**
  — callback-adapter matching `MmsReportClientCallback`'s signature exactly.
  `userParam` MUST be the `IpcDispatcherHandle` (cast back internally).
  Extracts, pairs quality, serializes to JSON, and enqueues onto the internal
  ring buffer — fast, non-blocking, never touches a `struct lws*` directly —
  safe to call from `mms_report_client`'s reconnect-supervisor thread. ALWAYS
  calls `MmsReportClient_destroyReportRecord(record)` before returning (takes
  ownership per that feature's callback contract) — the caller must NOT also
  destroy it.
- **`void IpcDispatcher_onGooseRecord(void* userParam, const GooseSubscriberRecord* record)`**
  — same contract, for GOOSE. Always calls
  `GooseSubscription_destroyRecord(record)` before returning. Safe to call
  from `goose_subscriber`'s `GooseReceiver` reception thread.

Both callbacks are thin wrappers: `ipc_dispatcher_api.c` just delegates to
`IpcDispatcherMmsAdapter_handleReport`/`IpcDispatcherGooseAdapter_handleRecord`
in `data/`.

## 3. Per-file breakdown

### `service/ipc_dispatcher_api.h` / `.c`
Public boundary and lifecycle wiring. `.c` is a thin composition layer: `_create`
allocates the handle and ring buffer; `_start` creates and starts the
`IpcDispatcherWsServer`; `_stop` destroys the ws server outright (see note
below); `_destroy` calls `_stop` then frees the ring buffer and handle. The two
`IpcDispatcher_on*` functions just cast `userParam` back to
`IpcDispatcherHandle` and delegate into the corresponding `data/` adapter.

Note on `_stop`: unlike `goose_subscriber`'s `GooseReceiver` (which can stop
its reception thread while keeping the socket open), an `lws_context`'s
listening socket has no "stop servicing, keep the bind" mode — so a clean
restart via a later `IpcDispatcher_start` requires fully releasing the port on
`_stop`, not just parking the thread.

### `domain/ipc_dispatcher_types.h`
Domain vocabulary — entirely feature-local (`IpcScalarValue`/`IpcQuality`/
`IpcMessage` are the JSON contract's own domain model), not borrowed from any
vendor lib. Zero third-party includes here by design (unlike
`mms_report_client`/`goose_subscriber`, this feature has two unrelated
third-party inputs — `MmsValue` — and two unrelated third-party outputs —
`cJSON`, `libwebsockets` — none of which is "the" domain; the one place
`MmsValue`/`Quality` are touched is `utils/ipc_dispatcher_value_codec.h`, and
`cJSON`/`libwebsockets` are confined to `data/`).

Key types:
- `IpcDispatcherError` — `OK`, `INVALID_ARGUMENT`, `OUT_OF_MEMORY`,
  `THREAD_CREATE_FAILED`, `SOCKET_BIND_FAILED`, `ALREADY_RUNNING`.
- `IpcQualityValidity` — mirrors `iec61850_common.h`'s `QUALITY_VALIDITY_*`
  2-bit field: `GOOD`, `RESERVED`, `INVALID`, `QUESTIONABLE`.
- `IpcQuality { validity; uint16_t detailFlags; }` — `detailFlags` is the raw
  `Quality` bitset (detail/test/operator-blocked/source-substituted/derived
  bits, plus the validity bits already named) copied verbatim rather than
  exposed as individually named booleans in v1 — additive later (a
  frontend-visible detail flag) isn't a breaking JSON-contract change, but
  un-inventing one would be.
- `IpcScalarType` — `BOOL`, `INT64`, `UINT64`, `DOUBLE`, `STRING`, `RAW`
  (fallback for an undecoded type or a `NULL` `MmsValue*`; `value.str` is an
  owned, human-readable placeholder like `"<unsupported:MMS_OCTET_STRING>"` or
  `"<null>"` — never silently dropped).
- `IpcScalarValue { type; union { bool b; int64_t i64; uint64_t u64; double d; char* str; } value; }`.
- `IpcSourceType` — `MMS_REPORT` / `GOOSE`.
- `IpcDataPoint` — one value entry with its paired `q` sibling (if any)
  already resolved: owned `reference` (the VALUE entry's own reference, never
  the `q` sibling's), `value`, `hasQuality`/`quality`,
  `hasPreviousValue`/`previousValue`, `hasPreviousQuality`/`previousQuality`,
  `hasLabel`/`label`, `hasPreviousLabel`/`previousLabel`. `label`/
  `previousLabel` are non-owned pointers into static string-literal storage
  (Dbpos labels only — see the value codec below) — never freed.
- `IpcDataPointExtras` — bundled optional per-point arrays
  (`pointHasPreviousValue`/`pointPreviousValue`/`pointHasPreviousQuality`/
  `pointPreviousQuality`/`pointHasLabel`/`pointLabel`/`pointHasPreviousLabel`/
  `pointPreviousLabel`), passed as one struct to
  `IpcDispatcherUseCases_assembleMessage` rather than growing that function's
  parameter list further. `NULL` (the whole struct, or any one array within
  it) means "no data for this field on any point."
- `IpcMessage` — one full outbound message: `sourceType`, owned
  `sourceReference` (rcbReference or goCbRef, may be `NULL`),
  `hasBuffered`/`buffered` (MMS only), `hasTimestamp`/`timestampMs`, owned
  `dataPoints` array + `dataPointCount`.
- `IpcDispatcherConfig { uint16_t port; int ringBufferCapacity; int maxConnections; }`.
- `struct sIpcDispatcherHandle` — defined here (not hidden behind an
  additional internal header) because every file in the feature needs field
  access, mirroring every sibling feature's `struct s*Handle` convention:
  `config`, owned `ringBuffer`, owned `wsServer` (`NULL` when not running),
  `volatile bool running`. `IpcDispatcherRingBuffer`/`IpcDispatcherWsServer`
  themselves are forward-declared only (bare tags, not `#include`d) so this
  header never pulls in `hal_thread.h`/`libwebsockets.h` — dependency
  direction stays `data/`/`utils/` → `domain/`, never reversed.

### `domain/ipc_dispatcher_usecases.h` / `.c`
Pure logic — zero third-party includes, unit-testable with hand-built plain
arrays only. Reference format assumed:
`"<LDName>/<LN>$<FC>$<DO>$<DA>"` (e.g. `LD0/LLN0$ST$Ind1$stVal` and
`LD0/LLN0$ST$Ind1$q` share everything up to the last `$`).

- **`IpcDispatcherUseCases_splitReference`** — splits a reference on its
  *last* `$` into a prefix length and a `daName` pointer (no allocation).
  Returns `false` if `reference` is `NULL` or has no `$`.
- **`findQualityIndexForValue`** (static) — the ancestor-prefix walk. Finds
  entry `valueIndex`'s quality sibling by walking its own reference's
  ancestor prefixes from deepest (closest) to shallowest, one `$`-segment at
  a time, stopping at the first ancestor level that has a `q` sibling among
  `references`. This is what makes a value's own reference depth irrelevant:
  a flat attribute (e.g. `...Pos$stVal`) finds its `q` one level up
  (`...Pos$q`) on the very first try — same result a naive single
  last-`$`-strip would give. A deeply nested CONSTRUCTED-DA chain (e.g. a
  CMV's `...PhV$phsA$cVal$mag$f`) walks past `...PhV$phsA$cVal$mag` and
  `...PhV$phsA$cVal` (neither has its own `q`) before finding
  `...PhV$phsA$q` several segments up — quality belongs to the whole CDC
  instance (`phsA`), not to whichever BDA happens to be the terminal leaf of
  one nested DA within it. A single last-`$`-strip (the original
  implementation, before this fix) only ever found quality for flat
  attributes, silently leaving every nested measured value (most measurands
  are CMV-shaped) with `quality: null` — confirmed against real production
  traffic. Returns `-1` if no ancestor level has a `q` sibling at all.
- **`IpcDispatcherUseCases_pairQuality`** — pairs `count` raw per-entry
  reference strings (`references[i]` may be `NULL`; this *includes* `q`
  entries at their own original index). A `q`-named entry is never itself
  emitted as a value entry (whether or not it has a matching sibling — a lone
  `q` is simply dropped, not fabricated into a value-less data point). Every
  other entry (non-`q` `daName`, or an unparseable/`NULL` reference) is
  emitted as a value entry, in original index order, paired via
  `findQualityIndexForValue`. Writes `outValueIndices`/
  `outQualityIndexForValue` (room for `count` elements each), returns the
  number of value entries written. `O(count² * depth)` — fine for realistic
  dataset sizes (tens of entries, shallow nesting).
- **`IpcDispatcherUseCases_assembleMessage`** — deep-copies already-paired,
  already-converted parallel arrays into one owned `IpcMessage`. Does no
  pairing itself — callers (the `data/` adapters) run `_pairQuality` first,
  convert the relevant `MmsValue`s via the value codec, then call this. Every
  string is deep-copied (`pointReferences[k]`, and
  `pointValues[k]`/`extras->pointPreviousValue[k].value.str` for STRING/RAW)
  — caller's own arrays/strings may be freed immediately after this returns.
  `sourceReference` may be `NULL` (copied through as `NULL`). `extras` may be
  `NULL` (or any individual array within it). `label`/`previousLabel` strings
  are copied *by pointer*, never duplicated (static string-literal storage) —
  the one field that breaks from this function's otherwise-uniform
  deep-copy rule. Returns `NULL` only on the top-level allocation failing; an
  individual string dup failing leaves that one field `NULL`.
- **`IpcDispatcherUseCases_freeMessage`** — frees an assembled `IpcMessage`:
  every data point's owned `reference`/`value` string (STRING/RAW only),
  `previousValue` string if owned, the `dataPoints` array, `sourceReference`,
  and the message itself. `label`/`previousLabel` are never freed (static
  storage). `NULL`-safe.

### `data/ipc_dispatcher_goose_adapter.h` / `.c`
`IpcDispatcherGooseAdapter_handleRecord(handle, record)` — the real logic
behind `IpcDispatcher_onGooseRecord`. Given a `GooseSubscriberRecord`:
1. Builds a flat `refs[]` array from `record->entries[i].reference` and runs
   `IpcDispatcherUseCases_pairQuality`.
2. For each paired value entry: converts its `MmsValue` via
   `IpcDispatcherValueCodec_convert`; converts `previousValue` the same way if
   present (`pointHasPreviousValue[k] = record->entries[vi].previousValue !=
   NULL`); attempts a Dbpos label via
   `IpcDispatcherValueCodec_decodeDbposLabel` for both current and previous
   value — if a label is found, **overrides** the raw numeric value with
   `(uint64_t) Dbpos_fromMmsValue(...)` instead of the generic
   `MmsValue_getBitStringAsInteger` decode, because the two disagree on bit
   order for a genuine Dbpos bitstring (confirmed empirically — ordinals 1/2
   come out swapped) — this keeps `value` and `label` from ever contradicting
   each other in the JSON; decodes quality from the paired `q` entry (current
   and previous) via `IpcDispatcherValueCodec_decodeQuality` if a quality
   index was found.
3. Bundles everything into an `IpcDataPointExtras` and calls
   `IpcDispatcherUseCases_assembleMessage` with `IPC_SOURCE_GOOSE`,
   `record->goCbRef` as `sourceReference`, `hasBuffered=false`,
   `hasTimestamp=true` (GOOSE always has a frame timestamp),
   `record->timestampMs`.
4. Serializes via `IpcDispatcherJsonWriter_write`, frees the intermediate
   `IpcMessage`, and — if JSON was produced — pushes it onto the ring buffer
   (`IpcDispatcherRingBuffer_push`, ownership transfers) and wakes the ws
   server (`IpcDispatcherWsServer_wake`).
5. Frees every intermediate array/scalar it allocated, then unconditionally
   calls `GooseSubscription_destroyRecord(record)` — the callback-ownership
   contract holds even if `handle` or `record` is `NULL`, or if any
   intermediate allocation failed.

Structurally a near-exact duplicate of the MMS adapter below (deliberate — no
shared "common adapter" file exists in this codebase's convention).

### `data/ipc_dispatcher_mms_adapter.h` / `.c`
`IpcDispatcherMmsAdapter_handleReport(handle, record)` — the real logic
behind `IpcDispatcher_onMmsReport`. Identical structure to the GOOSE adapter
above, operating on `MmsReportRecord` instead:
`IPC_SOURCE_MMS_REPORT`, `record->rcbReference` as `sourceReference`,
`hasBuffered=true`/`buffered=record->buffered`,
`hasTimestamp=record->hasTimestamp`/`record->timestampMs`. Always calls
`MmsReportClient_destroyReportRecord(record)` before returning.

Contains one extra, temporary piece: `appendDebugLog(path, text)` — a
diagnostic helper (duplicated from the identical helper in
`mms_report_client_report_adapter.c`, per this codebase's own
don't-share-across-features convention) that appends
`[<Hal_getTimeInMs()>] <text>` lines to a log file. Here it's called once,
logging the exact JSON string about to be handed to the ring buffer to
`/tmp/ied_reporter_debug_mms_websocket.log`. This is pending real-hardware
confirmation of the EntryID fix described in `mms_report_client`'s own
Architecture bullet — see CLAUDE.md's "Current State" section — and is
intended to be removed once confirmed. Note: the path logged to here
(`/tmp/ied_reporter_debug_mms_websocket.log`) differs from the one CLAUDE.md
names for `mms_report_client`'s own debug points
(`ied_reporter_debug_entryid.log`) — this is a separate, third debug log
point specific to this adapter's own JSON-serialization step.

### `data/ipc_dispatcher_json_writer.h` / `.c`
The only file in this feature that touches `cJSON.h`. One function:
`IpcDispatcherJsonWriter_write(const IpcMessage* message) -> char*` — returns
a heap-allocated, NUL-terminated JSON string (`cJSON_PrintUnformatted` — no
pretty-printing, this is a wire payload); caller owns it (`free()`). Returns
`NULL` if `message` is `NULL` or on allocation failure. No parse counterpart
exists anywhere in this feature (push-only).

**Envelope shape** (stable contract — CLAUDE.md: "treat message shape as a
stable contract, flag breaking changes explicitly"):

```json
{
  "schemaVersion": 1,
  "type": "MMS_REPORT" | "GOOSE",
  "source": { "rcbReference": "...", "buffered": true }   // MMS_REPORT
          | { "goCbRef": "..." },                          // GOOSE
  "hasTimestamp": true,
  "timestampMs": 1751520000123,                            // present only if hasTimestamp
  "dataPoints": [
    {
      "reference": "...",
      "value": <bool | number | string>,
      "quality": { "validity": "GOOD", "detailFlags": 0 } | null,
      "previousValue": <bool | number | string> | null,
      "previousQuality": { "validity": "...", "detailFlags": 0 } | null,
      "label": "<string>" | null,
      "previousLabel": "<string>" | null
    }
  ]
}
```

Details:
- `source.buffered` is only emitted for `MMS_REPORT`, and only if
  `message->hasBuffered` (always true in practice for MMS — GOOSE never sets
  it). `source.rcbReference`/`source.goCbRef` are emitted as JSON `null` if
  the underlying `sourceReference` was `NULL`.
- `"quality"`/`"previousValue"`/`"previousQuality"`/`"label"`/`"previousLabel"`
  are *always present* in the object, `null` when absent — a deliberate
  design decision (not merely omitted), so the frontend never has to
  distinguish "missing key" from "no value."
- Scalar JSON encoding (`buildScalarValueJson`): `BOOL`→JSON bool,
  `INT64`/`UINT64`→JSON number (cast to `double`), `DOUBLE`→JSON number,
  `STRING`/`RAW`→JSON string (`value.str`, or `""` if somehow `NULL`).
- **Known caveat** (documented in the header): cJSON numbers are
  double-backed, so an `INT64`/`UINT64` value beyond 2^53 loses precision —
  accepted as fine for GGIO indications/measurements (booleans/small
  integers/floats) reachable today.
- `dataPoints` is always emitted as an array (possibly empty), never omitted
  or `null`.

### `data/ipc_dispatcher_ring_buffer.h` / `.c`
Bounded, thread-safe broadcast ring of pre-serialized JSON strings — fixed
capacity, no dynamic growth (CLAUDE.md's "no dangling connections" ethos:
bounded resources, not unbounded queues). Opaque outside this file
(`struct sIpcDispatcherRingBuffer` is private to the `.c`), unlike every
sibling feature's `struct s*Handle` convention — nothing else needs field
access.

Struct: `slots` (owned `char*[capacity]`), `capacity`, `head` (`uint64_t`,
total messages ever pushed == next sequence number to write), `Semaphore
lock` (a `hal_thread.h` binary mutex, `Semaphore_create(1)` — same convention
`goose_subscriber` uses for its own `targetStateLock`; `hal_thread.h` has no
separate `Mutex` type).

- **`_create(capacity)`** — `NULL` on `capacity <= 0` or allocation failure.
- **`_destroy(buffer)`** — frees every still-buffered string plus the ring
  itself. `NULL`-safe.
- **`_push(buffer, json)`** — producer side. Takes ownership of `json`
  unconditionally (a `NULL` buffer just frees it — producer must never touch
  `json` again after calling, including on failure). `O(1)`, never blocks:
  stores `json` at slot `head % capacity`, frees whatever was previously in
  that slot (implicit drop-oldest — a lagging reader detects the gap on its
  own next `_readNext` call; this call itself never knows or cares how far
  behind any reader is), advances `head`. Held under the mutex for the whole
  operation (pointer swap only — bounded and fast).
- **`_readNext(buffer, cursor, outDroppedForThisRead)`** — reader side.
  `*cursor` is the caller's own next-unread sequence number (initialized to
  `_headSeq()` at connection-establish time — start-from-now, no backlog
  replay). If `*cursor == head`: nothing new, returns `NULL`,
  `*outDroppedForThisRead = 0`. If `head - *cursor > capacity` (this reader's
  next-unread message was already overwritten): fast-forwards `*cursor` to
  `head - capacity`, sets `*outDroppedForThisRead` to the number of messages
  skipped, and proceeds — so this call always returns the *oldest message
  still actually available*, never `NULL` purely due to lag. Otherwise:
  returns a freshly heap-allocated *copy* of the message at `*cursor`
  (independent of the ring's own slot — safe to hold across multiple partial
  writes even if the producer later overwrites that slot) and advances
  `*cursor` by 1. Safe to call concurrently with `_push` from a different
  thread; not safe to call for the *same* cursor from more than one thread at
  a time (a non-issue in practice — each connection's cursor is only ever
  touched by the one service-loop thread).
- **`_headSeq(buffer)`** — current head sequence number, used to initialize a
  new connection's cursor.
- **`_totalPushes(buffer)`** — alias for `_headSeq` — observational/metrics
  only.

Producer side: called from `mms_report_client`'s reconnect-supervisor thread
AND `goose_subscriber`'s `GooseReceiver` reception thread — two independent
producer threads. Reader side: called only from `ipc_dispatcher`'s own
libwebsockets service-loop thread, once per connected client per writable
opportunity.

### `data/ipc_dispatcher_ws_server.h` / `.c`
Owns the libwebsockets `struct lws_context*` and the *one* dedicated thread
that may ever touch it after creation — `lws_write`/`lws_service`/etc. are
not safe to call concurrently across threads for the same context; only
`lws_cancel_service` is documented safe to call from any thread, which is
exactly the cross-thread wakeup primitive this design relies on. Binds
`127.0.0.1` only — not caller-configurable to any other address. No TLS.
Opaque outside this file, same convention as the ring buffer.

Struct `sIpcDispatcherWsServer`: `context`, borrowed `ringBuffer`,
`protocols[2]` (`[1]` is the all-zero `LWS_PROTOCOL_LIST_TERM` terminator,
satisfied implicitly by `calloc`), `maxConnections`, `currentConnections`,
`volatile bool stopRequested`/`serviceExited`, `Thread serviceThread`.

Per-connection session data (`IpcDispatcherSession`, via lws's
`per_session_data_size`): just a `uint64_t cursor` into the shared ring
buffer. No pending-write buffer needed — `lws_write`'s own contract confirms
partial sends are buffered and retried autonomously by the library itself (an
extra `WRITEABLE` callback fires once the buffered remainder completes), so
the callback only ever issues at most one `lws_write` per `WRITEABLE` turn.

`ipcDispatcherCallback` (the one `lws_protocols` callback) handles:
- `LWS_CALLBACK_ESTABLISHED` — rejects (`return -1`) if
  `currentConnections >= maxConnections`; otherwise increments the count and
  initializes `session->cursor = IpcDispatcherRingBuffer_headSeq(...)` —
  start-from-now, no backlog replay for a newly connected client.
- `LWS_CALLBACK_CLOSED` — decrements `currentConnections`.
- `LWS_CALLBACK_EVENT_WAIT_CANCELLED` — fired by `lws_cancel_service`
  (either a producer's wake, or the stop path checked separately in the
  service loop). Calls `lws_callback_on_writable_all_protocol` to give every
  connection a writable turn; cheap — a connection with nothing new just
  no-ops on its own `WRITEABLE` turn, so this never busy-loops.
- `LWS_CALLBACK_SERVER_WRITEABLE` — calls `IpcDispatcherRingBuffer_readNext`
  with the session's cursor; if a message came back, `malloc`s a
  `LWS_PRE`-prefixed buffer, copies the payload in, calls `lws_write(...,
  LWS_WRITE_TEXT)`, frees the buffer and the JSON string. `dropped` (lagging
  messages skipped) is read but not surfaced over the wire in v1 (comment
  notes a future message `type` could report it). If the ring's head has
  since moved past this connection's (now-advanced) cursor, calls
  `lws_callback_on_writable(wsi)` again immediately — keeps draining without
  waiting for another external wake.

`serviceLoop` (the dedicated thread body): loops `lws_service(context,
1000)` until `stopRequested`. The `1000`ms figure is a bounded safety-net
timeout for `lws_service`'s own blocking wait, not a data-driving poll — real
wakeups come from `lws_cancel_service` (new message OR stop request).
Explicitly called out as transport-loop plumbing, not GOOSE/MMS data
reception, so it isn't a "no cyclic polling" Hard Rule violation — same class
of narrow exception as `goose_subscriber`'s liveness thread, narrower still
since it never gates or delays delivery itself.

Public functions:
- **`_create(port, maxConnections, ringBuffer, outError)`** — performs the
  actual bind (`lws_create_context` with `info.iface = "127.0.0.1"`,
  `info.port = port`). This is the synchronous I/O step, deliberately not
  done in `IpcDispatcher_create` (matches the "no I/O at create" contract
  shared with `mms_report_client`/`goose_subscriber`). `ringBuffer` is
  borrowed. Returns `NULL` + `IPC_DISPATCHER_ERR_SOCKET_BIND_FAILED` if
  `lws_create_context` fails (e.g. port already in use).
- **`_start(server)`** — starts the service-loop thread
  (`Thread_create`/`Thread_start`). Non-blocking. Returns
  `IPC_DISPATCHER_ERR_THREAD_CREATE_FAILED` only on `Thread_create` failure.
- **`_wake(server)`** — the one producer-thread-safe entry point: calls
  `lws_cancel_service(context)`. The *only* libwebsockets call a producer
  thread (`mms_report_client`'s supervisor / `goose_subscriber`'s reception
  thread) may make directly. Called by the `data/` adapters immediately after
  a successful ring-buffer push. Never blocks.
- **`_stop(server)`** — signals the service loop to exit
  (`stopRequested = true`), wakes it via `lws_cancel_service` to guarantee
  prompt exit even if idle, then blocks (`Thread_sleep(20)` poll loop) until
  `serviceExited`. Must be called from the caller's own thread, never from
  within an lws callback (deadlock). Safe to call more than once / before
  start (no-op).
- **`_destroy(server)`** — implies `_stop`, then `Thread_destroy`,
  `lws_context_destroy` (closes the listener + every connected client), and
  frees the handle. `NULL`-safe.

### `utils/ipc_dispatcher_value_codec.h` / `.c`
`MmsValue* → IpcScalarValue`/`IpcQuality` decode. Lives in `utils/`, not
`domain/`, precisely because it touches libiec61850's `MmsValue`/`Quality`
directly — the one shared third-party-touching helper used by both the MMS
and GOOSE `data/` adapters.

- **`IpcDispatcherValueCodec_convert(const MmsValue* value) -> IpcScalarValue`**
  — dispatches on `MmsValue_getType(value)`:
  - `MMS_BOOLEAN` → `IPC_SCALAR_BOOL` (`MmsValue_getBoolean`).
  - `MMS_INTEGER` → `IPC_SCALAR_INT64` (`MmsValue_toInt64`).
  - `MMS_UNSIGNED` → `IPC_SCALAR_UINT64` (`MmsValue_toUint32`, widened — no
    `MmsValue_toUint64` exists in the vendored header; a 32-bit ceiling on
    unsigned values is a known v1 limitation).
  - `MMS_FLOAT` → `IPC_SCALAR_DOUBLE` (`MmsValue_toDouble`).
  - `MMS_VISIBLE_STRING`/`MMS_STRING` → `IPC_SCALAR_STRING`
    (`MmsValue_toString`, `strdup`'d).
  - `MMS_UTC_TIME` → `IPC_SCALAR_UINT64` (`MmsValue_getUtcTimeInMs`) —
    unexpected as a plain "value" entry (the record-level timestamp is
    separate) but handled defensively, not asserted against.
  - `MMS_BIT_STRING` → `IPC_SCALAR_UINT64`
    (`MmsValue_getBitStringAsInteger`, little-endian bit order per that
    function's own doc comment) — covers CODEDENUM-typed value DAs (e.g.
    `Dbpos`/`Tcmd`) that wire-encode as a bitstring. Never quality's own
    bitstring — `pairQuality` excludes every `q`-named entry from ever being
    treated as a value, routing it to `_decodeQuality` instead. **Always a
    raw unsigned integer, never a decoded semantic label**, for every
    CODEDENUM subtype uniformly (Dbpos, Tcmd, or otherwise) — this function
    has no way to know which specific CODEDENUM a given bitstring represents,
    so guessing a decoded label without per-type verification would violate
    this repo's own "don't guess IEC 61850 semantics" rule; the raw bit
    pattern is always correct regardless of which CODEDENUM it is. **Known
    caveat for genuine Dbpos values specifically**: `MmsValue_getBitStringAsInteger`'s
    bit order does *not* match `Dbpos_fromMmsValue`'s own decode (confirmed
    empirically — `DBPOS_OFF`/`DBPOS_ON` come out swapped between the two).
    Callers that also call `_decodeDbposLabel` and get `true` back must
    override this raw value with `(uint64_t) Dbpos_fromMmsValue(value)`
    instead, so the numeric value and the label never contradict each other —
    both `data/` adapters do exactly this.
  - Everything else (`MMS_STRUCTURE`/`MMS_ARRAY`/`MMS_OCTET_STRING`/
    `MMS_GENERALIZED_TIME`/`MMS_BINARY_TIME`/`MMS_BCD`/`MMS_OBJ_ID`/
    `MMS_DATA_ACCESS_ERROR`) → `IPC_SCALAR_RAW`, `value.str` = owned
    `"<unsupported:...>"` placeholder — structure/array nesting is out of
    scope since dataset FCDA entries are individual DA leaves, not whole DOs.
  - `value == NULL` (legitimate for `GooseSubscriberEntry.value`) →
    `IPC_SCALAR_RAW`, `value.str = "<null>"`.
  - STRING/RAW variants are always `strdup`'d — never a dangling/unowned
    pointer into the source `MmsValue`.
- **`IpcDispatcherValueCodec_freeScalar(scalar)`** — frees a scalar produced
  by `_convert` (only STRING/RAW own anything). Safe on any value this module
  produced, including non-owning variants (no-op then).
- **`IpcDispatcherValueCodec_decodeQuality(value, outQuality) -> bool`** —
  decodes a `q` entry's wire-encoded `MMS_BIT_STRING` via
  `Quality_fromMmsValue(value)`, maps `Quality_getValidity` to
  `IpcQualityValidity`, copies the full raw `Quality` bitset (including the
  validity bits) verbatim into `outQuality->detailFlags`. Returns `false`
  (`outQuality` zeroed) if `value` is `NULL` or not `MMS_BIT_STRING` — caller
  must check before treating the entry as quality.
- **`IpcDispatcherValueCodec_decodeDbposLabel(semantic, value, outLabel) -> bool`**
  — decodes a genuine Dbpos-typed value into its IEC 61850-7-3 label
  (`intermediate-state`/`off`/`on`/`bad-state`, via `Dbpos_fromMmsValue`).
  `*outLabel` is set to a pointer into static string-literal storage (never
  owned, never freed by the caller). Returns `false` (`outLabel` untouched)
  unless `semantic == IED_MODEL_DA_SEMANTIC_DBPOS` AND `value` is non-`NULL`
  AND `MmsValue_getType(value) == MMS_BIT_STRING` — the only place in this
  codebase that decodes a CODEDENUM bitstring into a named label, and only
  because the caller has already verified (via `ied_model`'s SCL-derived
  semantic hint, threaded through `MmsReportEntry.semantic`/
  `GooseSubscriberEntry.semantic`) that this specific bitstring genuinely *is*
  a Dbpos, not a guess from the wire type alone (`Tcmd` shares the same wire
  representation but a different meaning). Does not replace the existing raw
  `IPC_SCALAR_UINT64` value from `_convert` — both are always emitted side by
  side in `IpcDataPoint` (`value` and `label`).

## 4. Threading & concurrency model

Three thread roles interact through exactly two synchronization points (the
ring buffer's mutex, and `lws_cancel_service` as a cross-thread wakeup):

1. **Producer threads** — `mms_report_client`'s reconnect-supervisor thread
   (calling `IpcDispatcher_onMmsReport`) and `goose_subscriber`'s
   `GooseReceiver` reception thread (calling `IpcDispatcher_onGooseRecord`).
   Both must never block — libwebsockets requires all `lws_write`/context
   access to happen on the one `lws_service()` thread, so a producer thread
   is only ever allowed to: (a) extract/pair/convert data (pure CPU, no I/O),
   (b) serialize to JSON via `IpcDispatcherJsonWriter_write`, (c) push the
   JSON string onto the ring buffer (`IpcDispatcherRingBuffer_push` — O(1),
   mutex-protected pointer swap, never blocks on I/O), and (d) call
   `IpcDispatcherWsServer_wake` (`lws_cancel_service` — the one libwebsockets
   call documented safe from any thread). Never touches a `struct lws*`.
2. **The lws service-loop thread** — the one thread ipc_dispatcher itself
   owns (`serviceLoop` in `ipc_dispatcher_ws_server.c`), created in
   `IpcDispatcherWsServer_start`. Runs `lws_service(context, 1000)` in a loop
   until told to stop. This is the *only* thread that ever calls
   `lws_write`/touches `struct lws*`/reads the ring buffer
   (`IpcDispatcherRingBuffer_readNext`). It's woken either by a producer's
   `_wake()` call (`LWS_CALLBACK_EVENT_WAIT_CANCELLED` → marks every
   connection writable) or by its own 1000ms safety-net timeout (not a data
   poll — see the file breakdown above). On each connection's
   `LWS_CALLBACK_SERVER_WRITEABLE` turn, it drains the ring buffer via that
   connection's own read cursor, one message per turn, re-arming itself
   immediately if more is buffered so it doesn't wait for another external
   wake.
3. **The caller's own thread** — whichever thread calls
   `IpcDispatcher_start`/`_stop`/`_destroy` (via `orchestration`/
   `device_manager`). `_stop`/`_destroy` block until the service-loop thread
   has actually exited; calling either from *inside* a producer callback or
   from inside the lws callback itself would deadlock (documented
   requirement, not enforced in code).

**The ring buffer is the only data handoff between producer and consumer
threads.** It is guarded by a single `hal_thread.h` `Semaphore` used as a
binary mutex (`Semaphore_create(1)`), held only for the pointer-swap duration
of a push or a copy-out — no I/O ever happens while the lock is held. Two
independent producer threads (MMS supervisor, GOOSE receiver) may call
`_push` concurrently; both are serialized safely by that one mutex. The
service-loop thread is the only reader, and each connected client has its
*own* cursor into the same shared ring — so multiple slow/fast clients don't
interfere with each other's reads, only with how much backlog they can each
still see.

**Lagging-client behavior**: the ring is fixed-capacity
(`ringBufferCapacity`, default 256) — a push always succeeds by overwriting
the oldest slot (drop-oldest, never blocks the producer). A reader
(connection) whose cursor has fallen more than `capacity` messages behind has
its cursor silently fast-forwarded to the oldest still-available message on
its next read (`IpcDispatcherRingBuffer_readNext`'s `outDroppedForThisRead`
path) — there is no backlog replay and no error surfaced to that client in
v1; it simply resumes from whatever is oldest still in the ring. A brand-new
connection starts its cursor at the current head (`_headSeq()` at
`LWS_CALLBACK_ESTABLISHED`) — it never sees anything pushed before it
connected.

## 5. Known limitations / deliberate scope boundaries

- **Changes-only stream, at the whole daemon's design level** (not specific
  to this feature's code, but this is where it's enforced): the genuine
  first-ever GI/bootstrap snapshot (and GOOSE's first-ever frame per target)
  never reaches `ipc_dispatcher` at all — `mms_report_client`/
  `goose_subscriber`'s own value-diff caches silently seed on `cached ==
  NULL` and never forward that observation. `dataPoints` only ever contains
  points that actually changed, judged at the (value, quality) *pair* level.
  See CLAUDE.md's "IPC / Reporting Out" section for the full rule.
- **No backlog replay for a new or lagging connection.** A newly connected
  client starts at the current ring head (nothing before connect-time). A
  client that falls more than `ringBufferCapacity` messages behind silently
  skips forward to the oldest still-buffered message — the skip count
  (`outDroppedForThisRead`) is computed internally but not currently surfaced
  in the JSON envelope (noted as a possible future `type`/field addition).
- **Loopback-only, no TLS.** `IpcDispatcherWsServer_create` hardcodes
  `info.iface = "127.0.0.1"` — not caller-configurable to any other address.
  The "Fan-out is broadcast to every connected control client" tradeoff
  documented for `control_dispatcher` applies here too in spirit: this
  feature trusts that only local, already-authorized processes can reach
  `127.0.0.1:8765`.
- **32-bit ceiling on `MMS_UNSIGNED`** — `MmsValue_toUint32` widened to
  `uint64_t`; no `MmsValue_toUint64` exists in the vendored
  `third_party/include` header.
- **cJSON's double-backed numbers** — an `INT64`/`UINT64` scalar beyond 2^53
  loses precision once serialized. Accepted as fine for the
  booleans/small-integers/floats reachable from real GGIO indications/
  measurements today.
- **Structure/array MMS types are out of scope** — `MMS_STRUCTURE`/
  `MMS_ARRAY`/etc. fall back to an `"<unsupported:...>"` placeholder string,
  never decoded, because dataset FCDA entries are individual DA leaves (Gap-4
  decomposition, done upstream in `mms_report_client`, already flattens
  structured attributes before they reach this feature).
- **No second consumer possible.** `orchestration` registers exactly one
  `IpcDispatcherHandle`'s callbacks per IED; there's no fan-out mechanism to
  register a second internal consumer of the same `MmsReportRecord`/
  `GooseSubscriberRecord` stream (only the external websocket fan-out to
  multiple *clients*, which is a different layer).
- **Temporary debug logging** — `ipc_dispatcher_mms_adapter.c`'s
  `appendDebugLog` call writes every serialized MMS-report JSON to
  `/tmp/ied_reporter_debug_mms_websocket.log`, pending real-hardware
  confirmation of `mms_report_client`'s EntryID fix (see CLAUDE.md's
  "Current State"). Intended to be removed once confirmed.

## 6. Cross-feature dependencies

- **Called by**: `src/orchestration/` only — `Orchestration_run*` creates one
  `IpcDispatcherHandle` per IED pipeline and registers
  `IpcDispatcher_onMmsReport`/`IpcDispatcher_onGooseRecord` as the callbacks
  for `mms_report_client`/`goose_subscriber` respectively, unconditionally,
  every run. `device_manager` doesn't call into `ipc_dispatcher` directly —
  it goes through `orchestration`, one instance per registered device, each
  on its own auto-assigned port.
- **Consumes**: `MmsReportRecord`/`MmsReportEntry` from
  `src/features/mms_report_client/service/mms_report_client_api.h`, and
  `GooseSubscriberRecord`/`GooseSubscriberEntry` from
  `src/features/goose_subscriber/service/goose_subscriber_api.h`. Also reads
  `IedModelDaSemantic` from `src/features/ied_model/service/ied_model_api.h`
  (via `record->entries[i].semantic`, threaded through from `ied_model`'s
  SCL-derived semantics table) to gate the Dbpos-label decode.
  `MmsReportEntry`/`GooseSubscriberEntry` are otherwise unmodified — this
  feature only reads them, then owns and destroys the full record via the
  matching `_destroyReportRecord`/`_destroyRecord` call.
- **Consumed by**: nothing inside this codebase — the JSON it emits over
  websocket is consumed entirely externally (the "high-level API and
  frontend" per CLAUDE.md), out of this repo's scope.
- **No dependency on** `scan_dispatcher`/`control_dispatcher` — those are
  structurally similar (own ring buffer + lws service thread) but entirely
  separate instances serving different data (scan results, control
  acks/errors) on different ports; deliberately duplicated, not shared (no
  precedent in this codebase for a cross-feature "shared" directory).

## 7. Tests

### `tests/ipc_dispatcher/` (unit, Unity, `cd tests && make run`)
- **`test_ipc_dispatcher_usecases.c`** — pure domain logic, no third-party
  includes touched. Covers `IpcDispatcherUseCases_splitReference` (normal
  split, `NULL`, no-`$` cases) and `IpcDispatcherUseCases_pairQuality`:
  flat value+`q` pairing, value-only (no `q` in dataset), a lone `q` with no
  sibling being dropped, the nested-CMV-chain case finding quality several
  ancestor levels up, a check that pairing doesn't overreach past a
  genuinely unrelated ancestor, unparseable/`NULL` references passed through
  as values, multiple independent groups not cross-mixing, and the
  zero-count edge case. Also covers `IpcDispatcherUseCases_assembleMessage`
  (deep-copy-not-alias, string-scalar deep copy, `NULL` sourceReference
  passthrough, no-quality case, `extras == NULL` leaving every `has*` flag
  false, and a full extras-populated round trip) and
  `IpcDispatcherUseCases_freeMessage` on `NULL`.
- **`test_ipc_dispatcher_value_codec.c`** — `IpcDispatcherValueCodec_convert`
  for boolean/integer/float/visible-string (deep-copied)/`NULL`
  (raw-null-placeholder)/unsupported-type (raw-placeholder)/bit-string
  (raw-unsigned-integer, including a zero-value case), plus
  `IpcDispatcherValueCodec_decodeQuality` for good, questionable-with-test-flag,
  `NULL` value, and wrong-MMS-type cases.
- **`test_ipc_dispatcher_ring_buffer.c`** — invalid-capacity rejection,
  `headSeq` starting at zero, `readNext` returning `NULL` when the cursor is
  caught up, push-then-readNext returning a copy (not an alias of the
  internal slot), wraparound dropping the oldest entry, two independent
  cursors where one lagging doesn't affect the other's own progress,
  push taking ownership even when the buffer itself is `NULL`, and
  `destroy` on `NULL`.
- **`test_ipc_dispatcher_json_writer.c`** — MMS report with quality, GOOSE
  omitting the `buffered` field, `hasTimestamp == false` omitting
  `timestampMs`, `hasQuality == false` emitting JSON `null`, previous
  value/quality fully populated, previous value/quality fully absent
  (JSON `null`), `NULL` sourceReference emitting JSON `null`, and `write`
  returning `NULL` on a `NULL` message.
- **`test_ipc_dispatcher_api.c`** — config defaults matching documented
  values (and not crashing on `NULL`), `create` applying defaults when
  config is `NULL`, `create` rejecting invalid capacities, `stop` before
  `start` being a no-op, double-`start` returning `ALREADY_RUNNING`, a real
  start→stop→start cycle cleanly reusing the same port, `destroy`/`stop` not
  crashing on a `NULL` handle, `start` returning `INVALID_ARGUMENT` on a
  `NULL` handle, and `onMmsReport`/`onGooseRecord` not crashing on a `NULL`
  record. Per the Makefile's own comment, this unit test does perform a real
  bind (justified there as acceptable for this specific suite).

`tests/Makefile` wires all five as explicit `TESTS` entries
(`test_ipc_dispatcher_usecases`, `test_ipc_dispatcher_value_codec`,
`test_ipc_dispatcher_ring_buffer`, `test_ipc_dispatcher_json_writer`,
`test_ipc_dispatcher_api`) with hand-written build rules pulling in exactly
the `.c` files each needs — no auto-discovery.

### `integration_tests/ipc_dispatcher/` (E2E, no `sudo`, `cd integration_tests/ipc_dispatcher && make run`)
- **`e2e_test_ipc_dispatcher.c`** — starts a real `IpcDispatcher` (real bind,
  real libwebsockets service thread), connects a hand-rolled minimal
  websocket client (raw TCP + HTTP-Upgrade handshake + a small RFC6455 frame
  parser for the unmasked/unfragmented text frames this server ever sends),
  drives real `MmsReportRecord`/`GooseSubscriberRecord` fixtures through
  `IpcDispatcher_onMmsReport`/`_onGooseRecord`, and asserts the real JSON
  received over the real socket matches the expected envelope (parsed back
  with cJSON, not raw string equality — resilient to key ordering).
  Deliberately does *not* use libwebsockets client mode for the test peer,
  even though it's already vendored — a bug shared by both client and server
  via the same library couldn't be caught that way (same "keep the fake peer
  decoupled from the code under test" philosophy `integration_tests/ied_simulator/`
  applies at the protocol level). `Sec-WebSocket-Accept` is intentionally not
  verified (would need hand-rolled SHA1 for no benefit to this test's actual
  goal); the handshake response's `101` status line is enough to know the
  upgrade succeeded. Two test cases:
  `test_mmsReport_withStValAndQ_arrivesAsPairedDataPoint` and
  `test_gooseRecord_arrivesWithGoCbRefSource`.

No `sudo` needed for either suite — everything here is loopback TCP/websocket,
no raw sockets.
