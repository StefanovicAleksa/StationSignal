# ied_reporter_daemon — API Guide for Agents

This is a self-contained reference for integrating against a running `ied_reporter_daemon`
instance as a network service. It assumes no access to the daemon's source — everything here
is the wire contract, confirmed directly against the code that produces it.

## What this is

A background daemon that sniffs IEC 61850 GOOSE traffic and subscribes to MMS report control
blocks (BRCB/URCB) on one or more substation IEDs, normalizes both into JSON, and streams them
out over websockets. **There is no REST/HTTP API** — every interaction, both control and data,
is a websocket connection. The daemon has no CLI and no config file read at startup; everything
is driven by the one control channel described below.

## Running it

The daemon is a single process, `ied_reporter_daemon`, built from this repo (see the repo's
`CLAUDE.md` for the exact build invocation — there's no packaged binary or install step yet).
It takes no arguments. On start it:

1. Binds the control websocket at `ws://127.0.0.1:8767`.
2. Blocks until `SIGINT`/`SIGTERM`.

Nothing else happens until a client sends a command. `sudo`/root is only required once a command
actually asks the daemon to talk to a device over GOOSE (raw Ethernet socket) — the process
itself starts and idles fine unprivileged.

All websockets are **loopback-only** (`127.0.0.1`), unauthenticated at the transport level (no
TLS), and use standard RFC6455 framing — a raw TCP client will not work, you need a real
websocket client library.

---

## 1. Control channel — `ws://127.0.0.1:8767`

The **only** way to start or stop anything. Bidirectional: you send JSON command messages, the
daemon pushes back one JSON response per command (and only per command — there's no unsolicited
push on this channel, e.g. no "device disconnected" notification).

Multiple clients may connect simultaneously; a response is **broadcast to every connected
client**, not just the one that sent the request — filter on `requestId` if more than one client
is connected at once.

### Inbound envelope

```json
{ "requestId": "<any string you choose>", "action": "<ACTION>", "params": { ... } }
```

- `requestId` — required, non-empty string. Echoed back verbatim on the response; this is how
  you correlate requests to responses (the daemon does not preserve message ordering guarantees
  beyond this).
- `action` — one of `START_REPORTING`, `STOP_REPORTING`, `START_SCAN`, `STOP_SCAN`.
- `params` — required object (may be empty `{}` only if the action needs no fields, which none
  currently do).

### Outbound envelope

```json
{
  "schemaVersion": 1,
  "requestId": "<echoed>",
  "action": "<echoed, or null if the action itself couldn't be determined>",
  "success": true,
  "result": { ... },
  "error": null
}
```

On failure, `success: false`, `result: null`, and `error` is populated:

```json
{ "code": "HOST_ALREADY_RUNNING", "message": "this host/mmsPort is already running or starting", "stage": null, "detail": null }
```

`stage`/`detail` are only ever non-null for a `START_REPORTING` failure whose `code` is
`ORCHESTRATION_FAILED` (see that action's error table below).

### Parse-level errors

If the message itself is malformed, `requestId` may be missing from the response (nothing to
echo):

| Condition | Behavior |
|---|---|
| Not valid JSON, or not a JSON object | connection-level rejection, no response frame |
| `requestId` missing/empty | rejected before any response can be built |
| `action` missing/unrecognized | `error.code: "UNKNOWN_ACTION"` (requestId still echoed if present) |
| `params` missing, not an object, or missing/invalid required fields for that action | `error.code` reflects the action-specific validation failure (see below) |

### `START_REPORTING`

Starts one IED's full MMS report + GOOSE pipeline and assigns it its own dedicated output
websocket.

**Request `params`:**

```json
{
  "host": "10.0.0.5",
  "mmsPort": 102,
  "iedName": null,
  "interfaceId": "eth0",
  "sclFilePath": null,
  "acseAuthPassword": null,
  "accessMode": "REPORT_ONLY"
}
```

| Field | Required | Notes |
|---|---|---|
| `host` | yes | non-empty string, the IED's IP/hostname |
| `interfaceId` | yes | non-empty string, the local NIC to use for GOOSE reception (e.g. `"eth0"`) |
| `mmsPort` | no | integer, defaults to `102` |
| `iedName` | no | omit/empty for auto-detect — **only works if the SCL declares exactly one `<IED>`**; fails hard (zero or multiple) otherwise. **Required** if `sclFilePath` is also given. |
| `sclFilePath` | no | a local filesystem path on the machine running the daemon. If given, the daemon skips network SCL discovery and loads this file directly — `iedName` becomes mandatory in this case. |
| `acseAuthPassword` | no | omit/empty for no ACSE-level auth |
| `accessMode` | no | one of `"REPORT_ONLY"`, `"READ_ONLY"`, `"READ_AND_WRITE"` — defaults to `"REPORT_ONLY"` if omitted; any other string is a parse error |

When `sclFilePath` is omitted (the common case), the daemon fetches the IED's SCL file itself
over MMS file services. If the device is connectable but never serves an SCL file (some real
devices, e.g. OMICRON IED Scout's "Simulate IED" mode), the daemon automatically falls back to
building the model by walking the device's live MMS data-model directory instead — this is
transparent, no extra step needed on the caller's side, just slower.

**Success `result`:**

```json
{ "deviceId": 1, "wsPort": 9000 }
```

`deviceId` is how you'll refer to this device in a later `STOP_REPORTING`. `wsPort` is the
per-device report-stream websocket port — connect to `ws://127.0.0.1:<wsPort>` to receive its
data (§3 below). Ports are auto-assigned from a `9000-9999` range.

**Error codes:**

| `error.code` | Meaning |
|---|---|
| `INVALID_ARGUMENT` | bad params (also returned for malformed JSON-level param issues before reaching device_manager) |
| `OUT_OF_MEMORY` | allocation failure |
| `PORT_EXHAUSTED` | no free websocket port left in the 9000-9999 range |
| `HOST_ALREADY_RUNNING` | this exact `(host, mmsPort)` is already running or mid-start — retry `STOP_REPORTING` first, or just don't double-start |
| `ORCHESTRATION_FAILED` | the pipeline itself failed partway (SCL fetch, MMS connect, GOOSE subscribe, etc). Check `error.stage` (one of the pipeline stages) and, if `stage` is `"BOOTSTRAP"`, `error.detail` (why the SCL-fetch stage specifically failed, e.g. no reachable host, auth rejected, no SCL file found) |
| `DEVICE_NOT_FOUND` | (not applicable to this action — reserved for `STOP_REPORTING`) |
| `START_IN_PROGRESS` | a start for this same `(host, mmsPort)` is already mid-flight on another request — retry shortly |

### `STOP_REPORTING`

```json
{ "deviceId": 1 }
```

`deviceId` — required non-negative integer, from a prior `START_REPORTING` response.

**Success `result`:** `{ "deviceId": 1 }`. The device's per-device websocket (§3) closes; its
port is freed for reuse by a future `START_REPORTING`.

**Errors:** `INVALID_ARGUMENT` (bad/missing `deviceId`), `DEVICE_NOT_FOUND` (unknown or
already-stopped `deviceId`).

There is **no unsolicited "device stopped" push** on any channel — if a device dies on its own
(connection loss, crash, etc.) nothing notifies you; the daemon does not watch connection health.
You only find out by trying to interact with it, or by design keeping your own liveness check on
the per-device stream.

### `START_SCAN`

Starts a continuous background subnet sweep on one interface, streaming discovered candidate
IEDs over the shared scan-result websocket (§4).

```json
{ "interfaceId": "eth0", "mmsPort": 102, "sweepIntervalMs": 0 }
```

| Field | Required | Notes |
|---|---|---|
| `interfaceId` | yes | non-empty string |
| `mmsPort` | no | defaults to `102` |
| `sweepIntervalMs` | no | `0` or omitted uses the daemon's own default sweep cadence |

**Success `result`:** `{ "scanId": 1 }`.

**Error codes:** `INVALID_ARGUMENT` (check `interfaceId`/`mmsPort`), `OUT_OF_MEMORY`,
`DISPATCHER_START_FAILED` (the shared scan-result websocket failed to bind — likely already in
use by something else), `THREAD_CREATE_FAILED`, `DISCOVERY_CREATE_FAILED`.

Note: no ACSE-password field on this action — a discovered device that turns out to need one
is only actually authenticated when you `START_REPORTING` on it.

### `STOP_SCAN`

```json
{ "scanId": 1 }
```

**Success `result`:** `{ "scanId": 1 }`.

**Errors:** `INVALID_ARGUMENT`, `SCAN_NOT_FOUND` (unknown or already-stopped `scanId`).

Stopping the last active scan tears down the shared scan-result websocket (§4) — a subsequent
`START_SCAN` rebinds it.

---

## 2. Report data model (applies to both output streams below)

Every reported data point carries this shape:

```json
{
  "reference": "Reporter1LD0/CSWI1$ST$Pos$stVal",
  "value": 1,
  "quality": { "validity": "GOOD", "detailFlags": 0 },
  "previousValue": 0,
  "previousQuality": { "validity": "GOOD", "detailFlags": 0 },
  "label": null,
  "previousLabel": null
}
```

- `value`/`previousValue` — one of: boolean, number (integers and floats both arrive as JSON
  numbers — MMS integers/unsigned/floats/UTC timestamps/bitstrings all normalize to a number or
  bool), or string. A value the daemon can't decode arrives as a string placeholder like
  `"<unsupported:MMS_OCTET_STRING>"` — treat any string starting with `<unsupported:` or equal to
  `<null>` as "not a real value."
- `quality`/`previousQuality` — `null` if this point has no sibling `q` attribute in the dataset;
  otherwise `{ "validity": "GOOD"|"INVALID"|"RESERVED"|"QUESTIONABLE", "detailFlags": <uint16> }`.
  `detailFlags` is the raw IEC 61850 Quality bitset (test/operator-blocked/source-substituted/
  derived/detail bits) — decode it bitwise if you need those, they aren't broken out individually.
- `label`/`previousLabel` — only non-null for a data point whose SCL type is genuinely `Dbpos`
  (double-point status): one of `"intermediate-state"`, `"off"`, `"on"`, `"bad-state"`. This is
  *additive* — `value` still carries the raw integer for every point, `Dbpos` or not. Every other
  point has `label`/`previousLabel: null`.
- `previousValue`/`previousQuality` are `null` only in the rare structural case where this
  position has no cache slot to diff against at all — not a routine occurrence once a device has
  been reporting for any length of time.

**The stream is changes-only.** The genuine first-ever observation of a data point (the initial
GI/bootstrap snapshot on first connect for MMS, the first-ever frame for a GOOSE target) is
never forwarded — it's silently used to seed the "what was the last value" cache. A point that
never changes again after that is never sent. After that first observation, every subsequent
report/frame is diffed against the last value *actually sent*, at the `(value, quality)` pair
level, and only forwarded if something changed. This means: if you connect and get nothing for a
point that never changes, that's expected, not a bug — the daemon does not resend unchanged
state periodically.

---

## 3. Per-device report stream — `ws://127.0.0.1:<wsPort>`

`<wsPort>` comes from `START_REPORTING`'s response. Push-only — the daemon never expects you to
send anything on this connection. One connection per consumer; every connected client on this
port gets the same broadcast stream (a lagging client's unread backlog is dropped, not queued
indefinitely — no replay).

### Envelope

```json
{
  "schemaVersion": 1,
  "type": "MMS_REPORT",
  "source": { "rcbReference": "Reporter1LD0/LLN0$BR$brcbMain", "buffered": true },
  "hasTimestamp": true,
  "timestampMs": 1752700800123,
  "dataPoints": [ /* array of data points, see §2 — only ones that changed */ ]
}
```

or, for GOOSE:

```json
{
  "schemaVersion": 1,
  "type": "GOOSE",
  "source": { "goCbRef": "Reporter1LD0/LLN0$GO$gcbStatus" },
  "hasTimestamp": true,
  "timestampMs": 1752700800456,
  "dataPoints": [ ... ]
}
```

- `type` — `"MMS_REPORT"` or `"GOOSE"`.
- `source` — for MMS: `{ "rcbReference": <string|null>, "buffered": <bool, only present for MMS> }`.
  For GOOSE: `{ "goCbRef": <string|null> }`.
- `hasTimestamp`/`timestampMs` — GOOSE messages always carry a timestamp (the frame's own publish
  time). MMS reports only carry one if the RCB's `OptFlds` requested it (`hasTimestamp: false`,
  `timestampMs` field absent, if not). This is one timestamp per message, not per data point.
- `dataPoints` — never empty in a forwarded message (an all-unchanged report simply isn't sent at
  all).

A single MMS report or GOOSE frame from the device can fan out into one message here with
multiple `dataPoints` if several dataset members changed together.

---

## 4. Scan-result stream — `ws://127.0.0.1:8766`

Shared across every concurrently-active scan (there is only ever one instance of this socket,
regardless of how many scans are running) — bound on the first `START_SCAN` (0→1 active scans),
torn down when the last one stops (1→0). Push-only.

### Envelope

```json
{
  "schemaVersion": 1,
  "type": "SCAN_RESULT",
  "scanId": 1,
  "host": "10.0.0.5",
  "mmsPort": 102,
  "discoveredAtMs": 1752700700000,
  "authRequired": false
}
```

One message per newly-discovered host (already-seen hosts within the same scan are not
re-announced). `scanId` tells you which `START_SCAN` call this came from if multiple scans are
active at once.

A discovered host here is only a **candidate** — it passed a TCP probe and a real (immediately
closed) MMS association, but nothing about its SCL/model has been fetched. To actually report on
it, send `START_REPORTING` with its `host`/`mmsPort`.

`authRequired: true` means this candidate answered MMS but the (unauthenticated) association
attempt was specifically access-denied, not that nothing is there — it's still a real IEC 61850
device, just not reachable without credentials. It's still worth showing to an operator: send
`START_REPORTING` with the same `host`/`mmsPort` plus the correct `acseAuthPassword` to connect.

---

## 5. Worked flows

### Flow A — start reporting on a known IED, read its stream

```
→ control (8767): {"requestId":"r1","action":"START_REPORTING",
    "params":{"host":"10.0.0.5","mmsPort":102,"interfaceId":"eth0"}}
← control (8767): {"schemaVersion":1,"requestId":"r1","action":"START_REPORTING","success":true,
    "result":{"deviceId":1,"wsPort":9000},"error":null}

→ connect ws://127.0.0.1:9000, then just listen:
← {"schemaVersion":1,"type":"MMS_REPORT","source":{"rcbReference":"...brcbMain","buffered":true},
    "hasTimestamp":true,"timestampMs":...,"dataPoints":[...]}
← {"schemaVersion":1,"type":"GOOSE","source":{"goCbRef":"...gcbStatus"},
    "hasTimestamp":true,"timestampMs":...,"dataPoints":[...]}
```

Nothing arrives until something on the device actually changes after your connection was
established (the initial snapshot is bootstrap-suppressed, per §2).

### Flow B — discover IEDs, then start reporting on one

```
→ control (8767): {"requestId":"s1","action":"START_SCAN",
    "params":{"interfaceId":"eth0","mmsPort":102}}
← control (8767): {"...","result":{"scanId":1},"error":null}

→ connect ws://127.0.0.1:8766, listen:
← {"schemaVersion":1,"type":"SCAN_RESULT","scanId":1,"host":"10.0.0.7","mmsPort":102,
    "discoveredAtMs":...,"authRequired":false}
← {"schemaVersion":1,"type":"SCAN_RESULT","scanId":1,"host":"10.0.0.9","mmsPort":102,
    "discoveredAtMs":...,"authRequired":true}

→ control (8767): {"requestId":"r2","action":"START_REPORTING",
    "params":{"host":"10.0.0.7","mmsPort":102,"interfaceId":"eth0"}}
← control (8767): {"...","result":{"deviceId":2,"wsPort":9001},"error":null}

→ control (8767): {"requestId":"s2","action":"STOP_SCAN","params":{"scanId":1}}
← control (8767): {"...","result":{"scanId":1},"error":null}
```

Scanning and reporting are independent — you don't need to stop the scan before reporting on a
discovered host, and the scan keeps running (and streaming new discoveries) in the background
until you explicitly stop it.

### Flow C — stop everything

```
→ control (8767): {"requestId":"x1","action":"STOP_REPORTING","params":{"deviceId":1}}
← control (8767): {"...","result":{"deviceId":1},"error":null}
   (ws://127.0.0.1:9000 closes)

→ control (8767): {"requestId":"x2","action":"STOP_SCAN","params":{"scanId":1}}
← control (8767): {"...","result":{"scanId":1},"error":null}
   (ws://127.0.0.1:8766 closes, since this was the last active scan)
```

---

## 6. Gotchas

- **Loopback-only, no TLS.** This is designed to sit behind a trusted local API layer, not be
  exposed directly. Don't put it on a network boundary as-is.
- **Broadcast fan-out on the control channel.** If two clients are connected to `:8767`
  simultaneously, both see every response, not just the requester — filter on `requestId`.
- **No unsolicited pushes anywhere.** No "device disconnected," no "scan finished" (scans don't
  finish on their own — they run until you `STOP_SCAN`). Silence on a stream means "nothing
  changed," not "something's wrong" — and conversely, the daemon does not tell you if a device's
  connection actually dropped; you'd only notice by the report stream going quiet.
  the underlying MMS/GOOSE connection state.
- **RFC6455 framing required.** A raw TCP socket sending bare JSON will not work on any of these
  ports — use a real websocket client.
- **`sclFilePath` is a path on the daemon's own filesystem**, not something you upload — if
  you're driving the daemon remotely, that file has to already exist on the daemon's host.
- **Ports 9000-9999 are finite.** `PORT_EXHAUSTED` is a real possible error once ~1000 devices
  are concurrently started; `STOP_REPORTING` frees a port for reuse.
