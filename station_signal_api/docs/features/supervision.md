# supervision

## 1. Overview

Owns the `station_signal_daemon` child process's entire lifecycle: spawning it, detecting
unexpected exit, respawning with backoff, and signaling readiness once its control channel is
actually accepting connections. Knows nothing about the daemon's wire contract — that's
`core/daemonclient`'s job.

## 2. Public API surface

`service.Supervisor` — the only type controllers or `main` are allowed to depend on:

```go
func New(binPath, controlAddr string, logger *slog.Logger) *Supervisor
func (s *Supervisor) Run(ctx context.Context) error
func (s *Supervisor) Restarts() <-chan struct{}
func (s *Supervisor) Running() bool
```

`Restarts()` fires once after the daemon is first spawned and ready, and again after every
subsequent respawn — `core/daemonclient.Client` is its sole consumer (see
`../API_OVERVIEW.md`'s crash re-arm section for why nothing else should watch this channel
directly).

## 3. Per-file breakdown

### `domain/policy.go`
Backoff tuning consts (`InitialBackoff`, `MaxBackoff`, `ReadyPollEvery`, `ReadyTimeout`,
`TermGracePeriod`) and the pure `NextBackoff(cur time.Duration) time.Duration` function
(double, capped at `MaxBackoff`). Zero I/O.

### `data/process.go`
Raw OS process mechanics: `Spawn` (exec + a goroutine feeding an exit channel from
`cmd.Wait()`), and `Process.Terminate`/`Process.Kill` (interrupt vs. SIGKILL). No restart
loop, no backoff — just "start one process"/"stop one process" primitives. `Process`'s
`Terminate`/`Kill` are nil-safe on a zero value, which `service`'s own unit tests rely on to
avoid needing a real process.

### `data/readiness.go`
`PollReady(ctx, addr, pollEvery, timeout) bool` — polls a TCP address until it accepts a
connection or gives up.

### `service/supervisor.go`
`Supervisor` — the `Run` loop that sequences `data.Spawn` → `data.PollReady` →
`domain.NextBackoff` on failure → respawn, holding `spawnFn`/`pollReadyFn` as unexported
fields (defaulted to the real `data` functions in `New`, overridable by unit tests in the same
package).

## 4. Threading & concurrency model

`Run` is meant to be started once in its own goroutine (`main` does `go sup.Run(ctx)`) and
owns the entire spawn/monitor loop sequentially — there's no concurrent access to the process
handle itself. `Running()`/`Restarts()` are the only cross-goroutine surface, both safe for
concurrent callers (`running` under a mutex; `restarts` a buffered channel).

## 5. Known limitations / deliberate scope boundaries

- Exact terminate→grace-period→kill escalation timing is only exercised against real
  processes in `data`'s own tests — `service`'s tests fake the process handle (a nil-safe
  zero-value `Process`) to test the *sequencing* logic without needing a real OS process or
  waiting out the real grace period.
- No configurable backoff/timeout values — the consts in `domain/policy.go` are fixed,
  matching this being a single-daemon-instance, single-substation deployment tool rather than
  something tuned per-environment.

## 6. Cross-feature dependencies

None — `supervision` doesn't depend on `reporting` or `scanning`, and isn't depended on by
them either. `main` wires `supervision.Supervisor.Restarts()` into
`core/daemonclient.New(...)`, and `controllers/rest`'s health handler reads
`supervision.Supervisor.Running()` directly (behind a local `daemonSupervisor` interface for
testability).

## 7. Tests

Unit: `domain/policy_test.go` (`NextBackoff`), `data/process_test.go` (spawn/terminate/kill
against tiny repo-local fixture scripts under `data/testdata/`, no root/daemon needed),
`data/readiness_test.go` (real local `net.Listen`), `service/supervisor_test.go` (backoff
doubling, restart-on-exit, ctx-cancellation shutdown — all via fake `spawnFn`/`pollReadyFn`,
no real process). No dedicated integration suite — supervision's actual behavior (respawn +
re-arm) is what `integration_tests/crashrearm/` exercises end-to-end.
