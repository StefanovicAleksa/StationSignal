# station_signal_api — API Guide for the Frontend

This is a self-contained reference for building a frontend against a running
`station_signal_api` instance. It assumes no access to this repo's source — everything here is
the wire contract, confirmed directly against the code that produces it. If you're a
context-free Claude session picking up the frontend work, read this before writing any client
code.

## What this is

A REST + WebSocket API that supervises a real `station_signal_daemon` process and relays
commands/data between it and you. **There is no direct connection to the daemon** — this API
is the only thing the frontend ever talks to. Everything here is JSON over HTTP or a plain
`ws://` websocket; no auth, no TLS (this stack is designed for one engineer/technician on one
substation's local network, not a multi-tenant or internet-facing deployment).

## Running it

The API is a single process, `station_signal_api`, that takes a required `-daemon-bin` flag (or
`STATION_SIGNAL_DAEMON_BIN` env var) pointing at a prebuilt daemon binary, and listens on
`-http-addr` (default `:8080`). It spawns and supervises the daemon itself — you never need to
start the daemon separately, and you never talk to its ports (8767, 8766, 9000-9999) directly.

All endpoints below are relative to the API's base URL, e.g. `http://<host>:8080`.

---

## 1. Health — `GET /api/health`

```json
{ "daemonRunning": true, "controlChannelConnected": true }
```

- `daemonRunning` — the daemon OS process is currently believed alive (supervised, may still
  be starting up).
- `controlChannelConnected` — the API's connection to the daemon's control channel is up,
  i.e. `/api/devices` and `/api/scans` calls will actually be able to reach the daemon right now.

Poll this before assuming the API is ready to accept device/scan commands, especially right
after the API itself has just started.

---

## 2. Device reporting — `/api/devices`

### `POST /api/devices` — start reporting on one IED

**Request body:**

```json
{
  "host": "10.0.0.5",
  "mmsPort": 102,
  "iedName": null,
  "interfaceId": "eth0",
  "sclFilePath": null,
  "acseAuthPassword": null,
  "accessMode": "REPORT_ONLY",
  "lnCategories": null
}
```

| Field | Required | Notes |
|---|---|---|
| `host` | yes | non-empty string, the IED's IP/hostname |
| `interfaceId` | yes | non-empty string, the local NIC on the machine running the daemon (e.g. `"eth0"`) |
| `mmsPort` | no | integer, defaults to `102` |
| `iedName` | no | omit for auto-detect — only works if the device's SCL declares exactly one `<IED>`; fails hard otherwise. Required if `sclFilePath` is given |
| `sclFilePath` | no | a path on the **daemon's own filesystem**. Either a path you already know exists there, or — more commonly — the `path` returned by `POST /api/structure-files` (§2a) after uploading a file from the browser |
| `acseAuthPassword` | no | omit for no ACSE-level auth |
| `accessMode` | no | one of `"REPORT_ONLY"`, `"READ_ONLY"`, `"READ_AND_WRITE"` — defaults to `"REPORT_ONLY"` |
| `lnCategories` | no | array of `"CONTROL"`, `"MEASUREMENT"`, `"PROTECTION"`, `"OTHER"` — filters which IEC 61850 Logical Node categories get subscribed to (e.g. `["CONTROL", "OTHER"]` skips measurement/protection data entirely). Omit (or `null`) for unfiltered — every LN, the previous/default behavior. An **empty array `[]` is rejected** with `400 INVALID_ARGUMENT` rather than treated as "unfiltered" — omit the field entirely if you want everything. An unrecognized category name is rejected the same way |

**Success (`201 Created`):**

```json
{ "deviceId": 1, "host": "10.0.0.5", "mmsPort": 102, "interfaceId": "eth0", "wsPort": 9000,
  "mmsAvailable": true, "gooseAvailable": true, "lnCategories": ["CONTROL", "OTHER"] }
```

`deviceId` identifies this session for `DELETE`/streaming. `wsPort` is an internal daemon
port — **ignore it**, you don't connect to it; use `/ws/devices/{deviceId}` instead (§4).

`lnCategories` is the filter the **running** device is actually subscribed with, which is not
necessarily the one you sent — see the device-sharing note below. Omitted entirely when the
device is unfiltered. Also included per-device in `GET /api/devices`.

`mmsAvailable`/`gooseAvailable` report which of MMS reporting / GOOSE subscription this
device's SCL actually declares — **a device only needs one of the two**, not both. A device
whose SCL has no `<ReportControl>` blocks starts with `mmsAvailable: false` (only GOOSE frames
will ever arrive on its stream); one with no `<GSEControl>` blocks starts with
`gooseAvailable: false` (only MMS reports will ever arrive). Neither case is an error — treat a
`false` here as a warning ("this device doesn't report over MMS/GOOSE"), not a failure. Only a
device with **neither** fails to start at all (see `ORCHESTRATION_FAILED` below, stage "no
capabilities"). These same two fields are also included per-device in `GET /api/devices` (§2
below), so a page reload/reconnect learns about a single-protocol device too, not just the
original `POST` response.

**Multiple sessions can watch the same physical device.** If another browser session already
has `host`/`mmsPort` active, this call attaches your session to that same device instead of
starting a second connection to the IED — you get back its existing `deviceId`/`wsPort` with
`201`, not an error. `DELETE /api/devices/{id}` only actually stops the physical device once
every attached session has called it; until then it just detaches your own session's view.

⚠️ **When you are attached rather than starting, the rest of your request body is discarded.**
The already-running device keeps the params its *creator* sent, so your own `lnCategories`,
`accessMode`, `sclFilePath` and `acseAuthPassword` have no effect — there is one physical
connection to that IED and one set of settings on it. The response is otherwise indistinguishable
from a real start, so **compare the `lnCategories` you sent against the `lnCategories` you get
back** to detect it, and tell the operator rather than silently showing them a different slice of
the device than the one they picked. (The frontend does exactly this; see its
`stores/devices.ts` `applyEffectiveCategories`.)

There is deliberately **no** way to open a second daemon connection to the same IED with a
different category filter. Two clients on one IED collide on the daemon's deterministic dataset
names, and each one's orphan cleanup will delete the other's live dataset server-side — see the
daemon's `CLAUDE.md` `device_manager` bullet for the full analysis. Narrowing what a given
technician *sees* is a client-side concern: every data point carries its own `category` (§4), so
filter the stream in the browser.

### `DELETE /api/devices/{id}` — stop reporting

**Success (`200 OK`):** `{ "deviceId": 1 }`. The device's stream (§4) closes.

### `DELETE /api/devices?host=&mmsPort=` — stop reporting by address (recovery path)

For the one case `DELETE /api/devices/{id}` can't help with: you never got, or have lost, a
`deviceId` for a device — most commonly, `POST /api/devices` returned `HOST_ALREADY_RUNNING` and a
follow-up `GET /api/devices` doesn't show a matching `host`+`mmsPort` either (the device is
genuinely running/starting on the daemon, this API just has no record of it — e.g. a prior request
that raced or timed out client-side). Query params: `host` (required), `mmsPort` (optional,
defaults to `102`).

**Success (`200 OK`):** `{ "host": "10.0.0.5", "mmsPort": 102 }`. Also succeeds (same `200`) if
nothing was actually registered at that address — an idempotent "already clean" outcome, not an
error, since this call exists specifically to clean up an unknown prior state.

**Errors specific to this endpoint:** `DEVICE_TRACKED` (`409`) if this API's own records already
know about a device at this address (yours or another session's) — use `DELETE /api/devices/{id}`
instead, this endpoint is only for genuinely untracked devices. `START_IN_PROGRESS` (`409`) if the
device exists on the daemon but is still mid-start from another in-flight request — no
cancellation exists; retry shortly.

### `GET /api/devices` — list every currently active device

```json
[
  { "deviceId": 1, "host": "10.0.0.5", "mmsPort": 102, "interfaceId": "eth0", "wsPort": 9000,
    "mmsAvailable": true, "gooseAvailable": true, "lnCategories": ["CONTROL", "OTHER"] }
]
```
Empty array (`[]`), not `null`, when nothing is active. Each entry is the same object
`POST /api/devices` returns — `mmsAvailable`/`gooseAvailable`/`lnCategories` all carry the meaning
they have above, including `lnCategories` being omitted entirely for an unfiltered device.

### Errors (all endpoints above)

See §5 for the full error-code → HTTP-status table. Malformed JSON or a missing/invalid
`{id}` path parameter both produce `400` with `code: "INVALID_ARGUMENT"` — likewise a missing
`host` query parameter on the address-based stop endpoint.

### `POST /api/structure-files` — upload an SCL/ICD/CID structure file

Lets a user pick or drag-and-drop a structure file in the browser and get back a path usable as
`sclFilePath` above — the file itself is saved on this API's own host (always the same box as
the daemon it supervises), never uploaded to the daemon directly.

**Request:** `multipart/form-data` with a single field named `file`. Accepted extensions:
`.icd`, `.cid`, `.scd`, `.xml` (case-insensitive) — anything else is rejected. No size limit is
enforced by this endpoint.

**Success (`201 Created`):**

```json
{ "path": "/var/lib/station_signal_api/structure_files/3f9a1c2d-device.icd" }
```

Pass this `path` straight through as `sclFilePath` in a subsequent `POST /api/devices` call — it's
already an absolute path the daemon can read. Each upload is stored under a fresh generated
name, so re-uploading a file with the same original filename never overwrites a previous one.

**Errors:** `400 INVALID_ARGUMENT` for a missing `file` field, an unsupported extension, or a
too-large upload; `500` (generic, no `code`) if the file couldn't be written to disk.

---

## 3. Scanning — `/api/scans`

Same shape as devices, for subnet scans:

### `POST /api/scans` — start a background subnet sweep

**Request body:**

```json
{ "interfaceId": "eth0", "mmsPort": 102, "sweepIntervalMs": 0 }
```

| Field | Required | Notes |
|---|---|---|
| `interfaceId` | yes | non-empty string |
| `mmsPort` | no | defaults to `102` |
| `sweepIntervalMs` | no | `0`/omitted uses the daemon's own default sweep cadence |

**Success (`201 Created`):** `{ "scanId": 1 }`.

### `DELETE /api/scans/{id}` — stop a scan

**Success (`200 OK`):** `{ "scanId": 1 }`. If this was the last active scan, the shared
scan-result stream (§4) closes for every subscriber.

### `GET /api/scans` — list every currently active scan

```json
[
  { "scanId": 1, "interfaceId": "eth0", "mmsPort": 102, "sweepIntervalMs": 0 }
]
```

---

## 4. Live data — websockets

Connect with a real WebSocket client (RFC6455 framing — a raw TCP/fetch call will not work).
Both endpoints are **push-only**: you never send anything on them.

### `ws://<host>:<port>/ws/devices/{deviceId}`

One connection per device you want to watch. Streams the same normalized JSON the daemon
itself produces, relayed verbatim — see §6 for the full data-point shape. Three message types:

```json
{
  "schemaVersion": 1,
  "type": "MMS_REPORT",
  "source": { "rcbReference": "Reporter1LD0/LLN0$BR$brcbMain", "buffered": true },
  "hasTimestamp": true,
  "timestampMs": 1752700800123,
  "dataPoints": [ /* see §6 — only points that changed */ ]
}
```

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

```json
{ "schemaVersion": 1, "type": "CONNECTION_STATUS", "status": "CONNECTED" }
{ "schemaVersion": 1, "type": "CONNECTION_STATUS", "status": "CONNECTION_REJECTED" }
```

This one is different in kind from the two above: it's a diagnostic/state signal, not report/GOOSE
data, and can arrive **after** `POST /api/devices` already returned `201` — the actual MMS report
association happens asynchronously in the background, so a device can look "started" well before
(or after) its report connection actually succeeds or gets rejected.

`status: "CONNECTED"` is the earliest honest signal that the device's MMS association actually
succeeded — use it to move the UI from "connecting" to "connected" instead of waiting for the
first report, which may never arrive (e.g. a device whose RCBs fail to enable reporting is still
genuinely connected). `status: "CONNECTION_REJECTED"` means the report connection was rejected
(e.g. it needs `acseAuthPassword` and none was given, or the wrong one was) — treat this as "no
reports may ever arrive on this device — a password may be required," and consider re-prompting
the user the same way as an `AUTH_REQUIRED` REST response (§5). **Important caveat, stated
honestly rather than glossed over**: `CONNECTION_REJECTED` reuses the underlying protocol
library's one "connection didn't succeed" code, which also covers plain transient network
failures, not exclusively wrong passwords — word your UI accordingly (e.g. "connection rejected —
check credentials" rather than asserting it's definitely a password problem). It fires at most
once per rejection streak, not on every retry.

- `404` (plain HTTP, connection never upgrades) if `{deviceId}` isn't currently active.
- The connection **closes** if the device is stopped (`DELETE /api/devices/{id}`) or the daemon
  crashes and hasn't been re-armed yet — reconnect and re-check `GET /api/devices` if you need to
  know whether it's still valid.
- **No replay for report/GOOSE data.** Only genuine changes are sent, and only from the moment
  you connect forward — the first-ever observation of any point is bootstrap-suppressed. Silence
  means nothing changed, not that something's wrong. **`CONNECTION_STATUS` is the one exception**:
  the most recent one is retained and replayed immediately to every new connection, specifically
  because it's a state (not a change event) and would otherwise almost always be missed — a real
  device's MMS association routinely succeeds before your websocket connection even opens.

### `ws://<host>:<port>/ws/scans`

One shared stream for every active scan. **Always accepts the connection**, even if no scan
is currently active — it idles until one starts, consistent with "silence means nothing
changed." Reconnect if you need to be sure you're not missing early results from a scan that
started before you connected (no replay here either).

```json
{
  "schemaVersion": 1,
  "type": "SCAN_RESULT",
  "scanId": 1,
  "host": "10.0.0.5",
  "mmsPort": 102,
  "discoveredAtMs": 1752700700000
}
```

One message per newly-discovered host. A discovered host is only a **candidate** — nothing
about its data model has been fetched. To actually report on it, `POST /api/devices` with its
`host`/`mmsPort`.

---

## 5. Errors

Every REST error response has this shape:

```json
{ "error": { "code": "HOST_ALREADY_RUNNING", "message": "...", "stage": null, "detail": null } }
```

`stage`/`detail` are only ever non-null for `code: "ORCHESTRATION_FAILED"` — `stage` is the
pipeline stage that failed, `detail` is further context if `stage == "SCL bootstrap"` (e.g. no
reachable host, auth rejected, no SCL file found). One `stage` value worth calling out:
`"no capabilities"` means the device's SCL declares neither `<ReportControl>` nor `<GSEControl>`
blocks at all — nothing to monitor, a genuine error. Contrast this with `mmsAvailable`/
`gooseAvailable` being `false` on a **successful** `POST /api/devices` (§2 above), which means
the device has only one of the two, not neither — that case is not an error.

| HTTP status | `error.code` | Meaning |
|---|---|---|
| 400 | `INVALID_ARGUMENT` | bad/missing request field, malformed JSON body, or invalid `{id}` path param |
| 401 | `AUTH_REQUIRED` | `POST /api/devices` was rejected because the device needs `acseAuthPassword` (missing or wrong) — see below |
| 404 | `DEVICE_NOT_FOUND` / `SCAN_NOT_FOUND` | unknown or already-stopped id |
| 409 | `HOST_ALREADY_RUNNING` | rare: another session already watching this `(host, mmsPort)` is the normal case and does **not** produce this error (see the sharing note in §2) — this surfaces when the daemon has this address registered but this API's own bookkeeping doesn't know about it (a process restart, or a prior request that raced/timed out client-side); recover with `DELETE /api/devices?host=&mmsPort=` (see above), then retry the connect |
| 409 | `START_IN_PROGRESS` | a start for this same target is already in flight — retry shortly |
| 409 | `DEVICE_TRACKED` | `DELETE /api/devices?host=&mmsPort=` only, when this API already has a record for that address — use `DELETE /api/devices/{id}` instead |
| 502 | `ORCHESTRATION_FAILED` | the device pipeline itself failed (see `stage`/`detail`) |
| 503 | `OUT_OF_MEMORY`, `PORT_EXHAUSTED`, `DISPATCHER_START_FAILED`, `THREAD_CREATE_FAILED`, `DISCOVERY_CREATE_FAILED` | daemon-side resource/infra failure |
| 503 | `DAEMON_UNREACHABLE` | this API couldn't reach the daemon at all (down, restarting, or the call timed out) — check `GET /api/health` |
| 500 | anything else / an unexpected internal error | a bug on this side, not a daemon-reported condition — the message body is deliberately generic, won't leak internals |

`DAEMON_UNREACHABLE` deserves special handling in the UI: it means transient infrastructure
trouble (the daemon crashed and is being respawned, typically resolves within a few seconds),
not a request-level problem — consider a retry-with-backoff rather than surfacing it as a
hard failure the way you'd treat `400`/`404`.

**`AUTH_REQUIRED`**: synthesized by this API itself, not part of the daemon's own error
vocabulary — it recognizes the daemon's `ORCHESTRATION_FAILED`/`"SCL bootstrap"`/access-denied
signature (the normal case: connecting by host, with no `sclFilePath`) and reports it as this
distinct code so you don't have to pattern-match a free-text `detail` string yourself. On
receiving it, prompt the user for a password and retry the same `POST /api/devices` call with
`acseAuthPassword` set. This only covers the discovery-connect flow; if you supply
`sclFilePath` yourself, a wrong/missing password on that path is not currently classified this
way (see the per-device stream note in §4 for the one case that still applies there).

---

## 6. Data point shape (inside every `dataPoints` array)

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

- `value`/`previousValue` — boolean, number, or string. A value the daemon couldn't decode
  arrives as a string like `"<unsupported:MMS_OCTET_STRING>"` or `"<null>"` — treat any
  string matching either pattern as "not a real value," not something to render as-is.
- `quality`/`previousQuality` — `null` if this point has no quality attribute; otherwise
  `{ "validity": "GOOD"|"INVALID"|"RESERVED"|"QUESTIONABLE", "detailFlags": <uint16 bitset> }`.
- `label`/`previousLabel` — only non-null for a double-point status (`Dbpos`) type: one of
  `"intermediate-state"`, `"off"`, `"on"`, `"bad-state"`. `value` still carries the raw
  integer either way.

---

## 7. Worked flow

```
GET /api/health
  -> {"daemonRunning":true,"controlChannelConnected":true}

POST /api/scans {"interfaceId":"eth0"}
  -> 201 {"scanId":1}

connect ws://host:8080/ws/scans, listen:
  <- {"type":"SCAN_RESULT","host":"10.0.0.7","mmsPort":102,...}

POST /api/devices {"host":"10.0.0.7","interfaceId":"eth0"}
  -> 201 {"deviceId":2,"wsPort":9001}   // ignore wsPort

connect ws://host:8080/ws/devices/2, listen:
  <- {"type":"MMS_REPORT","dataPoints":[...]}

DELETE /api/scans/1   -> 200 {"scanId":1}   // scanning and reporting are independent;
                                             // stopping the scan doesn't affect device 2
DELETE /api/devices/2 -> 200 {"deviceId":2} // ws://.../ws/devices/2 closes
```

## 8. Gotchas

- **No replay on either websocket.** Reconnecting after a gap means you missed whatever
  happened while disconnected — there's no history endpoint.
- **`wsPort` in a `POST /api/devices` response is not for you.** It's an internal daemon detail;
  always use `/ws/devices/{deviceId}` on this API instead.
- **`sclFilePath` is a path on the daemon's own filesystem**, not something the frontend
  uploads.
- **`DAEMON_UNREACHABLE` is usually transient**, not a hard failure — see §5.
- **This API and the daemon it wraps are both loopback/local-network-trust-model tools.**
  Don't assume auth, TLS, or multi-tenant isolation exist anywhere in this stack.
