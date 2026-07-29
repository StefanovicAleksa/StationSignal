// Package service is network's public façade — the only thing controllers/main may depend on
// for network-reconfiguration behavior. It wires domain's types together with data's privileged
// Runner and unprivileged StatusReader.
package service

import (
	"context"
	"errors"
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
	// applying is held for the duration of a runner.Apply shell-out. Without it, two concurrent
	// requests could both pass the "nothing pending" check and both invoke the helper, and the
	// second one's snapshot of "previous config" would capture the first one's already-modified
	// profile — so a later revert would restore the wrong address.
	applying bool

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
//
// The pending change is read from the helper's on-disk state, not just this process's memory.
// The two can disagree in both directions — an API restart wipes the in-memory copy while the
// on-disk marker survives (including across reboots), and that divergence is precisely what made
// a stuck change invisible to the technician while still blocking every apply.
func (s *Service) GetStatus(ctx context.Context) (domain.Status, error) {
	iface, current, err := s.statusReader.Current(ctx)
	if err != nil {
		return domain.Status{}, &domain.Error{Code: domain.ErrApplyFailed, Message: "failed to read current network configuration: " + err.Error()}
	}

	return domain.Status{
		Interface:       iface,
		Current:         current,
		RecoveryAddress: domain.RecoveryAddress,
		Pending:         s.pendingChange(ctx),
	}, nil
}

// pendingChange reconciles the helper's on-disk state with this process's in-memory record. Disk
// decides *whether* something is pending; memory only fills in details when it's describing that
// same change. If the helper can't be reached at all, memory is the fallback rather than a hard
// error — a status page that fails outright is worse than one reporting a slightly stale change.
func (s *Service) pendingChange(ctx context.Context) *domain.PendingChange {
	s.mu.Lock()
	inMemory := s.pending
	s.mu.Unlock()

	st, err := s.runner.Status(ctx)
	if err != nil {
		s.logger.Warn("could not read on-disk network state; falling back to in-memory", "error", err)
		return inMemory
	}
	if !st.Pending {
		return nil
	}
	if inMemory != nil {
		return inMemory
	}
	// Pending on disk but unknown to this process — i.e. it outlived an API restart. Reconstruct
	// what we can from what the helper recorded.
	pending := domain.PendingChange{New: domain.Config{CIDR: st.CIDR}, ExpiresAt: st.ExpiresAt}
	if st.Gateway != "" {
		gw := st.Gateway
		pending.New.Gateway = &gw
	}
	return &pending
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

	// Check and reserve under one lock: the reservation has to outlive the multi-second shell-out
	// below, or two concurrent applies both get through.
	s.mu.Lock()
	if s.pending != nil || s.applying {
		s.mu.Unlock()
		return domain.PendingChange{}, &domain.Error{Code: domain.ErrChangeAlreadyPending, Message: "a network change is already pending confirmation"}
	}
	s.applying = true
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		s.applying = false
		s.mu.Unlock()
	}()

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
		// The helper refusing because something is already in flight is a 409, not a 500 with raw
		// shell stderr in it — and it's a state the technician can clear from the Settings page
		// via Revert, so it must be reported as its own code rather than a generic failure.
		if errors.Is(err, data.ErrChangeAlreadyPending) {
			return domain.PendingChange{}, &domain.Error{Code: domain.ErrChangeAlreadyPending, Message: "a network change is already pending confirmation"}
		}
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

// Revert restores the configuration that was active before the last Apply and clears the
// helper's pending state. This is the technician's escape hatch from a change that never came
// up: without it, a pending marker the auto-revert failed to clear blocks every future apply and
// can only be removed with shell access to the box.
//
// It deliberately does not require this process to know about the change — the on-disk state is
// what actually blocks a retry, and it routinely outlives the in-memory copy (API restart,
// reboot). Only a confirmed-empty on-disk state makes this a no-op error; an unreadable one is
// treated as "try anyway", since refusing to run the recovery path because the box is in a bad
// state defeats the point.
func (s *Service) Revert(ctx context.Context) error {
	s.mu.Lock()
	inMemory := s.pending
	s.mu.Unlock()

	if inMemory == nil {
		if st, err := s.runner.Status(ctx); err == nil && !st.Pending {
			return &domain.Error{Code: domain.ErrNoPendingChange, Message: "no pending network change to revert"}
		}
	}

	if err := s.runner.Revert(ctx); err != nil {
		// The helper clears its state files even when the restore itself fails, so the wedge is
		// gone either way — but the technician still needs to hear that the box may not be back
		// on its previous address.
		s.clearPending()
		return &domain.Error{Code: domain.ErrApplyFailed, Message: "reverted, but the previous configuration could not be fully restored: " + err.Error()}
	}

	s.clearPending()
	return nil
}

// Reconcile clears a pending change orphaned by a reboot or crash — one whose OS-level
// auto-revert timer no longer exists to clear it, and which would otherwise refuse every apply
// for the lifetime of the box. Best-effort and called once at startup; a failure here must not
// stop the API from coming up.
func (s *Service) Reconcile(ctx context.Context) {
	if err := s.runner.Reconcile(ctx); err != nil {
		s.logger.Warn("could not reconcile leftover network state at startup", "error", err)
		return
	}
	s.clearPending()
}

func (s *Service) clearPending() {
	s.mu.Lock()
	s.pending = nil
	s.mu.Unlock()
}
