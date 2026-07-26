// Package service is reporting's public façade — the only thing controllers, main, or other
// features are allowed to depend on for device-reporting behavior. It wires domain's types
// together with data's daemon gateway and store.
package service

import (
	"context"
	"errors"
	"log/slog"

	"station_signal_api/internal/core/daemonclient"
	"station_signal_api/internal/core/daemonproto"
	"station_signal_api/internal/features/reporting/data"
	"station_signal_api/internal/features/reporting/domain"
)

// Service is reporting's public API.
type Service struct {
	gateway data.Gateway
	store   *data.Store
}

// New builds a Service backed by the real daemon gateway. hubCtx is the parent context for
// every device stream hub's upstream connection goroutine — it should live for the whole
// process lifetime and only be canceled at shutdown.
func New(client daemonclient.Caller, hubCtx context.Context, logger *slog.Logger) *Service {
	if logger == nil {
		logger = slog.Default()
	}
	return &Service{gateway: data.NewGateway(client, hubCtx, logger), store: data.NewStore()}
}

// Start issues START_REPORTING for params and records the resulting device.
func (s *Service) Start(ctx context.Context, params domain.StartParams) (domain.Device, error) {
	device, hub, err := s.gateway.Start(ctx, params)
	if err != nil {
		return domain.Device{}, err
	}
	s.store.Add(device, hub)
	return device, nil
}

// Stop issues STOP_REPORTING for deviceID and drops it from the active-device store. A
// DEVICE_NOT_FOUND failure still drops the store entry — the daemon has no record of this
// device either way, so the entry is stale bookkeeping rather than something worth retrying —
// while any other error leaves the store untouched so a genuinely transient failure can be
// retried.
func (s *Service) Stop(ctx context.Context, deviceID int) error {
	err := s.gateway.Stop(ctx, deviceID)
	if err != nil {
		var derr *daemonproto.Error
		if !errors.As(err, &derr) || derr.Code != daemonproto.ErrDeviceNotFound {
			return err
		}
	}
	if hub, ok := s.store.Remove(deviceID); ok && hub != nil {
		hub.Close()
	}
	return err
}

// List returns every currently active device.
func (s *Service) List() []domain.Device {
	return s.store.List()
}

// Snapshot returns the original start params of every active device, for crash re-arm.
func (s *Service) Snapshot() []domain.StartParams {
	return s.store.Snapshot()
}

// IsConnected reports whether host:mmsPort currently has an active reporting session.
func (s *Service) IsConnected(host string, mmsPort int) bool {
	return s.store.IsConnected(host, mmsPort)
}

// Clear drops every active device, closing their (already-dead) hubs — used only by crash
// re-arm immediately before replaying Snapshot() against a freshly restarted daemon.
func (s *Service) Clear() {
	s.store.Clear()
}

// StreamFor subscribes to an active device's stream hub. The returned cancel func must be
// called exactly once when the subscriber is done (e.g. on client disconnect).
func (s *Service) StreamFor(deviceID int) (<-chan []byte, func(), bool) {
	hub, ok := s.store.Hub(deviceID)
	if !ok {
		return nil, nil, false
	}
	ch, cancel := hub.Subscribe()
	return ch, cancel, true
}
