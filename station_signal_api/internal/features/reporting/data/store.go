// Package data is reporting's I/O layer: the mutex-guarded active-device store and the
// daemon-facing gateway calls. Imports domain for types; never imported by domain.
package data

import (
	"sync"

	"station_signal_api/internal/core/streamrelay"
	"station_signal_api/internal/features/reporting/domain"
)

type record struct {
	device domain.Device
	hub    *streamrelay.Hub
}

// Store is the mutex-guarded in-memory record of active devices and their stream hubs.
type Store struct {
	mu      sync.RWMutex
	devices map[int]*record
}

// NewStore builds an empty Store.
func NewStore() *Store {
	return &Store{devices: make(map[int]*record)}
}

// Add records a newly started device alongside its stream hub.
func (s *Store) Add(device domain.Device, hub *streamrelay.Hub) {
	s.mu.Lock()
	s.devices[device.ID] = &record{device: device, hub: hub}
	s.mu.Unlock()
}

// Remove drops a device and returns its hub (if any), for the caller to close.
func (s *Store) Remove(id int) (*streamrelay.Hub, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.devices[id]
	if !ok {
		return nil, false
	}
	delete(s.devices, id)
	return r.hub, true
}

// Get returns the device with the given id, if active.
func (s *Store) Get(id int) (domain.Device, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	r, ok := s.devices[id]
	if !ok {
		return domain.Device{}, false
	}
	return r.device, true
}

// Hub returns the stream hub for an active device.
func (s *Store) Hub(id int) (*streamrelay.Hub, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	r, ok := s.devices[id]
	if !ok {
		return nil, false
	}
	return r.hub, true
}

// IsConnected reports whether a device at host:mmsPort currently has an active reporting session.
func (s *Store) IsConnected(host string, mmsPort int) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, r := range s.devices {
		if r.device.Host == host && r.device.MMSPort == mmsPort {
			return true
		}
	}
	return false
}

// List returns a snapshot of all currently active devices, regardless of owning session —
// callers outside this package should use ListForSession instead; this stays for internal use
// (crash re-arm's Snapshot goes through the same map, see below).
func (s *Store) List() []domain.Device {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]domain.Device, 0, len(s.devices))
	for _, r := range s.devices {
		out = append(out, r.device)
	}
	return out
}

// ListForSession returns a snapshot of the active devices owned by sessionID.
func (s *Store) ListForSession(sessionID string) []domain.Device {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]domain.Device, 0, len(s.devices))
	for _, r := range s.devices {
		if r.device.SessionID == sessionID {
			out = append(out, r.device)
		}
	}
	return out
}

// Snapshot returns every active device (full record, including its owning SessionID), for
// crash re-arm to replay against a freshly restarted daemon under the same sessions that
// started them.
func (s *Store) Snapshot() []domain.Device {
	return s.List()
}

// Clear drops every device, closing each one's hub — used during crash re-arm, where the old
// hubs are already dead (their upstream connections died with the old daemon process) and get
// replaced wholesale rather than torn down individually.
func (s *Store) Clear() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, r := range s.devices {
		if r.hub != nil {
			r.hub.Close()
		}
	}
	s.devices = make(map[int]*record)
}
