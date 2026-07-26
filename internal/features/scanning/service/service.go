// Package service is scanning's public façade — the only thing controllers, main, or other
// features are allowed to depend on for scan behavior. It wires domain's types together with
// data's daemon calls and store.
package service

import (
	"context"
	"log/slog"

	"ied_reporter_api/internal/core/daemonclient"
	"ied_reporter_api/internal/features/scanning/data"
	"ied_reporter_api/internal/features/scanning/domain"
)

// Service is scanning's public API.
type Service struct {
	gateway data.Gateway
	store   *data.Store
}

// New builds a Service backed by the real daemon gateway. hubCtx is the parent context for
// the shared scan-result hub's upstream connection goroutine — it should live for the whole
// process lifetime and only be canceled at shutdown.
func New(client daemonclient.Caller, hubCtx context.Context, logger *slog.Logger) *Service {
	return &Service{gateway: data.NewGateway(client), store: data.NewStore(hubCtx, logger)}
}

// Start issues START_SCAN for params and records the resulting scan.
func (s *Service) Start(ctx context.Context, params domain.StartParams) (domain.Scan, error) {
	scan, err := s.gateway.Start(ctx, params)
	if err != nil {
		return domain.Scan{}, err
	}
	s.store.Add(scan)
	return scan, nil
}

// Stop issues STOP_SCAN for scanID and drops it from the active-scan store.
func (s *Service) Stop(ctx context.Context, scanID int) error {
	if err := s.gateway.Stop(ctx, scanID); err != nil {
		return err
	}
	s.store.Remove(scanID)
	return nil
}

// List returns every currently active scan.
func (s *Service) List() []domain.Scan {
	return s.store.List()
}

// Snapshot returns the original start params of every active scan, for crash re-arm.
func (s *Service) Snapshot() []domain.StartParams {
	return s.store.Snapshot()
}

// Clear drops every active scan, closing the (already-dead) shared hub — used only by crash
// re-arm immediately before replaying Snapshot() against a freshly restarted daemon.
func (s *Service) Clear() {
	s.store.Clear()
}

// StreamScans subscribes to the shared scan-result hub, if at least one scan is active. The
// returned cancel func must be called exactly once when the subscriber is done.
func (s *Service) StreamScans() (<-chan []byte, func(), bool) {
	hub, ok := s.store.Hub()
	if !ok {
		return nil, nil, false
	}
	ch, cancel := hub.Subscribe()
	return ch, cancel, true
}
