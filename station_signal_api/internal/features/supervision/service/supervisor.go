// Package service is supervision's public façade: Supervisor, the only type other features,
// controllers, or main are allowed to depend on for daemon-process lifecycle. It sequences
// data's process/readiness primitives using domain's backoff policy — it owns no I/O itself.
package service

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"station_signal_api/internal/features/supervision/data"
	"station_signal_api/internal/features/supervision/domain"
)

// Supervisor spawns and monitors a single station_signal_daemon process.
type Supervisor struct {
	binPath     string
	controlAddr string // e.g. "127.0.0.1:8767" — polled for readiness after each spawn
	// daemonLogLevel/daemonLogDir are handed to every spawn so the child's verbosity and log
	// location track this process's own — see data.Spawn for why they are passed explicitly
	// rather than inherited.
	daemonLogLevel string
	daemonLogDir   string
	logger         *slog.Logger

	// spawnFn/pollReadyFn default to data.Spawn/data.PollReady in New(). Unit tests in this
	// package construct a Supervisor{} literal directly with fakes here, driving the full
	// backoff/restart/ctx-cancellation loop without a real OS process.
	spawnFn     func(binPath string, daemonLogLevel string, daemonLogDir string, logger *slog.Logger) (*data.Process, <-chan error, error)
	pollReadyFn func(ctx context.Context, addr string, pollEvery, timeout time.Duration) bool

	mu      sync.Mutex
	running bool

	restarts chan struct{}
}

// New builds a Supervisor for the daemon binary at binPath, using controlAddr (the daemon's
// control-channel address) as the readiness probe target.
func New(binPath, controlAddr, daemonLogLevel, daemonLogDir string, logger *slog.Logger) *Supervisor {
	if logger == nil {
		logger = slog.Default()
	}
	return &Supervisor{
		binPath:        binPath,
		controlAddr:    controlAddr,
		daemonLogLevel: daemonLogLevel,
		daemonLogDir:   daemonLogDir,
		logger:         logger,
		spawnFn:        data.Spawn,
		pollReadyFn:    data.PollReady,
		// Buffered 1: if nobody has consumed the previous readiness signal yet, the newest
		// one is what matters — a listener only cares about "is it ready now."
		restarts: make(chan struct{}, 1),
	}
}

// Restarts fires once after the daemon is first spawned and ready, and again after every
// subsequent respawn (crash + successful restart). Callers distinguish "initial start" from
// "post-crash restart" themselves (e.g. by counting signals) since the channel carries no
// payload.
func (s *Supervisor) Restarts() <-chan struct{} {
	return s.restarts
}

// Running reports whether the supervised process is currently believed to be alive.
func (s *Supervisor) Running() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.running
}

// Run spawns the daemon and supervises it until ctx is canceled. On unexpected exit it
// backs off and respawns indefinitely. On ctx cancellation it stops the daemon (interrupt,
// then SIGKILL after a grace period) and returns.
func (s *Supervisor) Run(ctx context.Context) error {
	backoff := domain.InitialBackoff
	for {
		proc, exitCh, err := s.spawnFn(s.binPath, s.daemonLogLevel, s.daemonLogDir, s.logger)
		if err != nil {
			s.logger.Error("failed to spawn daemon", "error", err)
			if !sleepOrDone(ctx, backoff) {
				return ctx.Err()
			}
			backoff = domain.NextBackoff(backoff)
			continue
		}
		s.setRunning(true)

		if s.pollReadyFn(ctx, s.controlAddr, domain.ReadyPollEvery, domain.ReadyTimeout) {
			backoff = domain.InitialBackoff
			s.signalRestart()
		} else {
			s.logger.Error("daemon did not become ready in time", "controlAddr", s.controlAddr)
		}

		select {
		case <-ctx.Done():
			proc.Terminate()
			select {
			case <-exitCh:
				s.logger.Info("daemon exited gracefully after SIGTERM")
			case <-time.After(domain.TermGracePeriod):
				// Escalating past the graceful path: the daemon did not exit within
				// TermGracePeriod, so its own MmsReportClientConnection_stop teardown
				// (per-device dataset cleanup + ACSE Release) may not have finished for
				// every active device before SIGKILL lands — any device still connected
				// at this point may be left with an un-released MMS association on the
				// real IED. Logged explicitly so this is diagnosable from
				// station-signal-api.log instead of only inferable after the fact.
				s.logger.Warn("daemon did not exit within grace period after SIGTERM, escalating to SIGKILL - "+
					"active device connections may not have shut down cleanly on the remote IED(s)",
					"gracePeriod", domain.TermGracePeriod)
				proc.Kill()
				<-exitCh
			}
			s.setRunning(false)
			return ctx.Err()
		case err := <-exitCh:
			s.setRunning(false)
			s.logger.Warn("daemon process exited unexpectedly, will restart", "error", err)
			if !sleepOrDone(ctx, backoff) {
				return ctx.Err()
			}
			backoff = domain.NextBackoff(backoff)
		}
	}
}

func (s *Supervisor) signalRestart() {
	select {
	case s.restarts <- struct{}{}:
	default:
		// A previous unconsumed signal is still pending; it already means "ready."
	}
}

func (s *Supervisor) setRunning(v bool) {
	s.mu.Lock()
	s.running = v
	s.mu.Unlock()
}

func sleepOrDone(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-t.C:
		return true
	case <-ctx.Done():
		return false
	}
}
