# reporting

## 1. Overview

Owns `START_REPORTING`/`STOP_REPORTING` and the per-device stream hub lifecycle. Everything
the frontend sees under `/api/devices` and `/ws/devices/{id}` is this feature.

## 2. Public API surface

`service.Service` — the only type controllers or `main` are allowed to depend on:

```go
func New(client daemonclient.Caller, hubCtx context.Context, logger *slog.Logger) *Service
func (s *Service) Start(ctx context.Context, params domain.StartParams) (domain.Device, error)
func (s *Service) Stop(ctx context.Context, deviceID int) error
func (s *Service) List() []domain.Device
func (s *Service) Snapshot() []domain.StartParams
func (s *Service) Clear()
func (s *Service) StreamFor(deviceID int) (<-chan []byte, func(), bool)
```

`Snapshot`/`Clear` exist only for crash re-arm (see `../API_OVERVIEW.md`). `StreamFor` is how
`controllers/ws` subscribes to a device's data without ever seeing `core/streamrelay.Hub`
directly.

`domain.Device` and `domain.StartParams` (see `domain/device.go`) are also part of the public
contract — `Device`'s JSON tags are the exact shape `GET /api/devices` serializes.

## 3. Per-file breakdown

### `domain/device.go`
`Device` entity, `StartParams` value type, `AccessMode` enum, `DefaultMMSPort` const,
`EffectiveMMSPort` pure helper. Zero I/O, zero imports of `data/`/`service/`.

### `data/store.go`
Mutex-guarded `map[int]*record{device, hub}` — the in-memory record of active devices. Owns
no daemon-calling logic; `Add`/`Remove`/`Get`/`Hub`/`List`/`Snapshot`/`Clear` only.

### `data/gateway.go`
`Gateway` interface (`Start`/`Stop`) + `daemonGateway`, the concrete implementation wrapping a
`daemonclient.Caller`. Maps `domain.StartParams` ↔ `daemonproto.StartReportingParams`, issues
`START_REPORTING`/`STOP_REPORTING`, and — on a successful start — dials the device's stream
hub via `core/streamrelay.NewHub(hubCtx, "ws://127.0.0.1:<wsPort>", ...)`. The `Gateway`
interface exists purely so `service`'s unit tests can substitute a mock.

### `service/service.go`
`Service` — wires `data.Store` + `data.Gateway` together. `Start` calls the gateway, and only
on success stores the result; `Stop` calls the gateway, and only on success removes the entry
and closes its hub.

## 4. Threading & concurrency model

`data.Store` is `sync.RWMutex`-guarded; safe for concurrent REST handlers. Each device's
`streamrelay.Hub` owns its own upstream-read goroutine (started by `NewHub`, one per active
device) plus a broadcast-under-lock to however many frontend WS subscribers are attached.
`service.Service` itself holds no additional locking — all shared state lives in `data.Store`.

## 5. Known limitations / deliberate scope boundaries

- No independent validation of `StartParams` beyond what the daemon itself rejects with
  `INVALID_ARGUMENT` — this feature never re-implements the daemon's own validation, per the
  repo's Hard Rule against redesigning the daemon's contract.
- `wsPort` in `domain.Device` is carried through from the daemon's response but is not meant
  to be given to the frontend as something to dial directly — `FRONTEND_API_GUIDE.md` says so
  explicitly.
- **`AUTH_REQUIRED` classification lives in `controllers/rest` (`devices.go`'s
  `classifyStartReportingError`), not here.** This feature's own `gateway.go` still just
  relays whatever the daemon reports; the REST layer pattern-matches the daemon's
  `ORCHESTRATION_FAILED`/`"SCL bootstrap"`/access-denied signature into a distinct code purely
  for the frontend's convenience (see `FRONTEND_API_GUIDE.md` §5) — it does not change what
  this feature or the daemon actually do.
- **The daemon's per-device push stream can also carry a `CONNECTION_STATUS` message**
  (`{"type":"CONNECTION_STATUS","status":"CONNECTION_REJECTED"}`) after a successful
  `POST /api/devices`, if the device's actual MMS report connection (as opposed to the SCL
  bootstrap fetch) is later rejected — most commonly a device that requires
  `acseAuthPassword` on the report association specifically. This API relays it verbatim
  (`core/streamrelay.Hub` is a byte-for-byte opaque relay, no special-casing needed here).
  Honest caveat: the underlying signal is "the connection didn't succeed," not
  specifically "wrong password" — see `FRONTEND_API_GUIDE.md` §4's own caveat.
- This classification only covers the normal discovery-connect flow (no `sclFilePath`
  given). If the caller supplies `sclFilePath` directly, a rejected password on that path
  has no equivalent mitigating context (no prior bootstrap step already proved the host
  reachable) and is not specially classified — it still only ever surfaces via the
  `CONNECTION_STATUS` push above, if at all.

## 6. Cross-feature dependencies

Depends on `core/daemonclient` (via the `Caller` interface) and `core/streamrelay`. Consumed
by `controllers/rest` (`Start`/`Stop`/`List`), `controllers/ws` (`StreamFor`), and `main`'s
re-arm routine (`Snapshot`/`Clear`/`Start`). Never imports `scanning` or `supervision`.

## 7. Tests

Unit: `domain/device_test.go` (`EffectiveMMSPort`), `data/store_test.go` (store semantics, no
mocks needed), `data/gateway_test.go` (wire mapping + error passthrough, mock
`daemonclient.Caller`), `service/service_test.go` (orchestration logic, mock `data.Gateway` +
real `data.Store`). Integration: `integration_tests/reporting/` against the real daemon binary
(and, for the full success path, the daemon's own manual IED simulator) — sudo-gated, since
`START_REPORTING` needs the daemon's raw GOOSE socket.
