// Package service is reporting's public façade — the only thing controllers, main, or other
// features are allowed to depend on for device-reporting behavior. It wires domain's types
// together with data's daemon gateway and store.
package service

import (
	"context"
	"errors"
	"fmt"
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
	locks   *keyedLocks
}

// New builds a Service backed by the real daemon gateway. hubCtx is the parent context for
// every device stream hub's upstream connection goroutine — it should live for the whole
// process lifetime and only be canceled at shutdown.
func New(client daemonclient.Caller, hubCtx context.Context, logger *slog.Logger) *Service {
	if logger == nil {
		logger = slog.Default()
	}
	return &Service{gateway: data.NewGateway(client, hubCtx, logger), store: data.NewStore(), locks: newKeyedLocks()}
}

func lockKey(host string, mmsPort int) string {
	return fmt.Sprintf("%s|%d", host, mmsPort)
}

// Start attaches sessionID to the device at params.Host/params.MMSPort. If another session
// already has that physical device active, this attaches to it directly — no daemon call, the
// existing device is shared, and sessionID's later List/Stream/Stop calls affect only its own
// attachment. Otherwise this issues START_REPORTING and becomes the device's creator.
func (s *Service) Start(ctx context.Context, sessionID string, params domain.StartParams) (domain.Device, error) {
	host := params.Host
	port := domain.EffectiveMMSPort(params.MMSPort)

	unlock := s.locks.Lock(lockKey(host, port))
	defer unlock()

	if existing, ok := s.store.FindByHostPort(host, port); ok {
		device, _ := s.store.Attach(existing.ID, sessionID)
		return device, nil
	}

	device, hub, err := s.gateway.Start(ctx, params)
	if err != nil {
		return domain.Device{}, err
	}
	device.SessionID = sessionID
	s.store.Add(device, hub, sessionID)
	return device, nil
}

// Stop detaches sessionID from deviceID — but only if sessionID is currently attached to it. A
// device not attached to this session (whether owned solely by a different session or
// nonexistent) is rejected identically, as DEVICE_NOT_FOUND, so a session can't distinguish
// "not yours" from "doesn't exist" by probing IDs.
//
// If another session is still attached afterward, this is purely a local detach — the daemon is
// never called, and the device keeps running for the remaining session(s). Only when this is the
// last attached session does it issue STOP_REPORTING and close the shared stream hub. A
// DEVICE_NOT_FOUND failure from the daemon itself still drops this session's attachment (and the
// whole record, if it was the last one) — the daemon has no record of this device either way, so
// it's stale bookkeeping rather than something worth retrying — while any other error leaves the
// record and this session's attachment untouched so a genuinely transient failure can be retried.
func (s *Service) Stop(ctx context.Context, sessionID string, deviceID int) error {
	device, ok := s.store.Get(deviceID)
	if !ok {
		return &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound, Message: "device not found"}
	}

	unlock := s.locks.Lock(lockKey(device.Host, device.MMSPort))
	defer unlock()

	if !s.store.IsAttached(deviceID, sessionID) {
		return &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound, Message: "device not found"}
	}

	if s.store.AttachedSessionCount(deviceID) > 1 {
		s.store.Detach(deviceID, sessionID)
		return nil
	}

	err := s.gateway.Stop(ctx, deviceID)
	if err != nil {
		var derr *daemonproto.Error
		if !errors.As(err, &derr) || derr.Code != daemonproto.ErrDeviceNotFound {
			return err
		}
	}
	if hub, wasLast := s.store.Detach(deviceID, sessionID); wasLast && hub != nil {
		hub.Close()
	}
	return err
}

// ListForSession returns every currently active device attached to sessionID.
func (s *Service) ListForSession(sessionID string) []domain.Device {
	return s.store.ListForSession(sessionID)
}

// OwnsDevice reports whether sessionID is currently attached to deviceID.
func (s *Service) OwnsDevice(sessionID string, deviceID int) bool {
	return s.store.IsAttached(deviceID, sessionID)
}

// Snapshot returns every active device (one row per attached session, including its owning
// session), for crash re-arm.
func (s *Service) Snapshot() []domain.Device {
	return s.store.Snapshot()
}

// IsConnected reports whether sessionID currently has an active reporting attachment to the
// device at host:mmsPort — another session being attached to it doesn't count.
func (s *Service) IsConnected(sessionID, host string, mmsPort int) bool {
	return s.store.IsConnected(sessionID, host, mmsPort)
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
