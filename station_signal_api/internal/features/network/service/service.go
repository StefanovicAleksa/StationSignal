// Package service is network's public façade — the only thing controllers/main may depend on
// for network-reconfiguration behavior. It wires domain's types together with data's privileged
// Runner and unprivileged StatusReader.
package service

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"station_signal_api/internal/features/network/data"
	"station_signal_api/internal/features/network/domain"
)

// Service is network's public API.
type Service struct {
	mu      sync.Mutex
	pending *domain.PendingChange

	// countActiveReporting/countActiveScanning are narrow closures (not interfaces on
	// reportingsvc.Service/scanningsvc.Service) purely so this feature never needs to import
	// another feature's domain package — see main.go for how they're wired.
	countActiveReporting func() int
	countActiveScanning  func() int

	runner       data.Runner
	statusReader data.StatusReader

	revertTimeout time.Duration
	logger        *slog.Logger
}

// New builds a Service. revertTimeout <= 0 falls back to domain.DefaultRevertTimeoutSeconds.
func New(countActiveReporting, countActiveScanning func() int, runner data.Runner, statusReader data.StatusReader, revertTimeout time.Duration, logger *slog.Logger) *Service {
	if logger == nil {
		logger = slog.Default()
	}
	if revertTimeout <= 0 {
		revertTimeout = domain.DefaultRevertTimeoutSeconds * time.Second
	}
	return &Service{
		countActiveReporting: countActiveReporting,
		countActiveScanning:  countActiveScanning,
		runner:               runner,
		statusReader:         statusReader,
		revertTimeout:        revertTimeout,
		logger:               logger,
	}
}

// GetStatus reports the LAN interface's current configuration, the fixed recovery address, and
// any pending (not-yet-confirmed) change.
func (s *Service) GetStatus(ctx context.Context) (domain.Status, error) {
	iface, current, err := s.statusReader.Current(ctx)
	if err != nil {
		return domain.Status{}, &domain.Error{Code: domain.ErrApplyFailed, Message: "failed to read current network configuration: " + err.Error()}
	}

	s.mu.Lock()
	pending := s.pending
	s.mu.Unlock()

	return domain.Status{
		Interface:       iface,
		Current:         current,
		RecoveryAddress: domain.RecoveryAddress,
		Pending:         pending,
	}, nil
}

// Apply validates cfg, refuses if any device-reporting or scan session is currently active
// (their daemon sockets are bound to the interface's current address and would silently break),
// then provisionally applies it via the privileged Runner. The change is not permanent until a
// later Confirm — see deploy/scripts/station-signal-netconfig.sh for the OS-level auto-revert
// this schedules independently of this process.
func (s *Service) Apply(ctx context.Context, cfg domain.Config) (domain.PendingChange, error) {
	if err := cfg.Validate(); err != nil {
		return domain.PendingChange{}, err
	}

	s.mu.Lock()
	if s.pending != nil {
		s.mu.Unlock()
		return domain.PendingChange{}, &domain.Error{Code: domain.ErrChangeAlreadyPending, Message: "a network change is already pending confirmation"}
	}
	s.mu.Unlock()

	if n := s.countActiveReporting(); n > 0 {
		return domain.PendingChange{}, &domain.Error{Code: domain.ErrSessionsActive, Message: "stop all active device reporting sessions before changing the network configuration"}
	}
	if n := s.countActiveScanning(); n > 0 {
		return domain.PendingChange{}, &domain.Error{Code: domain.ErrSessionsActive, Message: "stop all active scans before changing the network configuration"}
	}

	gateway := ""
	if cfg.Gateway != nil {
		gateway = *cfg.Gateway
	}
	if err := s.runner.Apply(ctx, cfg.CIDR, gateway, int(s.revertTimeout.Seconds())); err != nil {
		return domain.PendingChange{}, &domain.Error{Code: domain.ErrApplyFailed, Message: "failed to apply network configuration: " + err.Error()}
	}

	pending := domain.PendingChange{New: cfg, ExpiresAt: time.Now().Add(s.revertTimeout)}
	s.mu.Lock()
	s.pending = &pending
	s.mu.Unlock()

	// The privileged helper already scheduled the real, process-independent OS-level revert
	// (systemd-run) — this timer only clears our own in-memory bookkeeping if nothing ever
	// confirms, so a later GetStatus stops claiming a change is pending long after the OS
	// already reverted it underneath us.
	expiresAt := pending.ExpiresAt
	time.AfterFunc(s.revertTimeout+5*time.Second, func() {
		s.mu.Lock()
		defer s.mu.Unlock()
		// Only clear if this is still the same pending change this timer was scheduled for —
		// Confirm (pending -> nil) or a later Apply (only possible once pending is nil) could
		// otherwise race with this firing.
		if s.pending != nil && s.pending.ExpiresAt.Equal(expiresAt) {
			s.pending = nil
		}
	})

	return pending, nil
}

// Confirm cancels the pending auto-revert and makes the last-applied change permanent.
func (s *Service) Confirm(ctx context.Context) error {
	s.mu.Lock()
	if s.pending == nil {
		s.mu.Unlock()
		return &domain.Error{Code: domain.ErrNoPendingChange, Message: "no pending network change to confirm"}
	}
	s.mu.Unlock()

	if err := s.runner.Confirm(ctx); err != nil {
		return &domain.Error{Code: domain.ErrApplyFailed, Message: "failed to confirm network configuration: " + err.Error()}
	}

	s.mu.Lock()
	s.pending = nil
	s.mu.Unlock()
	return nil
}
