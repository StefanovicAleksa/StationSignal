# scanning

## 1. Overview

Owns `START_SCAN`/`STOP_SCAN` and the shared scan-result hub's 0↔1 lifecycle. Everything the
frontend sees under `/scans` and `/ws/scans` is this feature.

## 2. Public API surface

`service.Service` — the only type controllers or `main` are allowed to depend on:

```go
func New(client daemonclient.Caller, hubCtx context.Context, logger *slog.Logger) *Service
func (s *Service) Start(ctx context.Context, params domain.StartParams) (domain.Scan, error)
func (s *Service) Stop(ctx context.Context, scanID int) error
func (s *Service) List() []domain.Scan
func (s *Service) Snapshot() []domain.StartParams
func (s *Service) Clear()
func (s *Service) StreamScans() (<-chan []byte, func(), bool)
```

Same shape as `reporting`'s service, with one structural difference: there is exactly one
shared hub (not one per scan), so `StreamScans` takes no id.

## 3. Per-file breakdown

### `domain/scan.go`
`Scan` entity, `StartParams` value type, `DefaultMMSPort` const, `EffectiveMMSPort` pure
helper. Zero I/O.

### `data/store.go`
Mutex-guarded `map[int]domain.Scan` **plus** the singleton scan-result hub pointer. Unlike
`reporting`'s store, this one owns hub lifecycle itself: `Add` dials
`core/streamrelay.NewHub(hubCtx, "ws://127.0.0.1:8766", ...)` on the 0→1 transition (first
scan starts), `Remove` closes it on the 1→0 transition (last scan stops) — mirroring exactly
what the daemon itself does with its own shared scan-dispatcher socket.

### `data/gateway.go`
`Gateway` interface (`Start`/`Stop`) + `daemonGateway` wrapping a `daemonclient.Caller`. Maps
`domain.StartParams` ↔ `daemonproto.StartScanParams`, issues `START_SCAN`/`STOP_SCAN`. No hub
dialing here (that's the store's job, since it's keyed off scan *count*, not any one scan).

### `service/service.go`
`Service` — wires `data.Store` + `data.Gateway` together, same shape as `reporting`'s.

## 4. Threading & concurrency model

`data.Store` is `sync.RWMutex`-guarded. The scan hub creation/teardown happens inside the same
locked section as the map mutation, so the 0↔1/1↔0 transition can't race two concurrent
`Add`/`Remove` calls into double-creating or double-closing the hub.

## 5. Known limitations / deliberate scope boundaries

- Discovered-host results aren't deduplicated or persisted by this feature beyond what the
  hub relays live — there's no "list of everything ever discovered" endpoint, matching the
  daemon's own "no replay" stream semantics.
- No independent validation beyond what the daemon rejects with `INVALID_ARGUMENT`, per the
  repo's Hard Rule against redesigning the daemon's contract.

## 6. Cross-feature dependencies

Depends on `core/daemonclient` (via the `Caller` interface) and `core/streamrelay`. Consumed
by `controllers/rest` (`Start`/`Stop`/`List`), `controllers/ws` (`StreamScans`), and `main`'s
re-arm routine. Never imports `reporting` or `supervision`.

## 7. Tests

Unit: `domain/scan_test.go`, `data/store_test.go` (including the 0↔1 hub-transition and
same-hub-shared-across-scans cases), `data/gateway_test.go` (mock `daemonclient.Caller`),
`service/service_test.go` (mock `data.Gateway` + real `data.Store`). Integration:
`integration_tests/scanning/` against the real daemon binary — no root needed, since scanning
uses a TCP probe + real MMS association rather than a raw socket.
