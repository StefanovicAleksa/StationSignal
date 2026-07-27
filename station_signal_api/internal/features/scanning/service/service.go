// Package service is scanning's public façade — the only thing controllers, main, or other
// features are allowed to depend on for scan behavior. It wires domain's types together with
// data's daemon calls and store.
package service

import (
	"context"
	"log/slog"

	"station_signal_api/internal/core/daemonclient"
	"station_signal_api/internal/core/daemonproto"
	"station_signal_api/internal/features/scanning/data"
	"station_signal_api/internal/features/scanning/domain"
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

// Start issues START_SCAN for params and records the resulting scan under sessionID, so only
// that session's later List/Stream/Stop calls can see or affect it.
func (s *Service) Start(ctx context.Context, sessionID string, params domain.StartParams) (domain.Scan, error) {
	scan, err := s.gateway.Start(ctx, params)
	if err != nil {
		return domain.Scan{}, err
	}
	scan.SessionID = sessionID
	s.store.Add(scan)
	return scan, nil
}

// Stop issues STOP_SCAN for scanID and drops it from the active-scan store — but only if
// sessionID is the one that started it. A scan owned by a different session (or one that
// doesn't exist) is rejected identically, as SCAN_NOT_FOUND, so a session can't distinguish
// "not yours" from "doesn't exist" by probing IDs.
func (s *Service) Stop(ctx context.Context, sessionID string, scanID int) error {
	if !s.OwnsScan(sessionID, scanID) {
		return &daemonproto.Error{Code: daemonproto.ErrScanNotFound, Message: "scan not found"}
	}
	if err := s.gateway.Stop(ctx, scanID); err != nil {
		return err
	}
	s.store.Remove(scanID)
	return nil
}

// ListForSession returns every currently active scan owned by sessionID.
func (s *Service) ListForSession(sessionID string) []domain.Scan {
	return s.store.ListForSession(sessionID)
}

// OwnsScan reports whether sessionID is the session that started scanID.
func (s *Service) OwnsScan(sessionID string, scanID int) bool {
	sc, ok := s.store.Get(scanID)
	return ok && sc.SessionID == sessionID
}

// Snapshot returns every active scan (including its owning session), for crash re-arm.
func (s *Service) Snapshot() []domain.Scan {
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
