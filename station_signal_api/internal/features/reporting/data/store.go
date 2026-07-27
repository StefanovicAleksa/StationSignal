// Package data is reporting's I/O layer: the mutex-guarded active-device store and the
// daemon-facing gateway calls. Imports domain for types; never imported by domain.
package data

import (
	"sync"

	"station_signal_api/internal/core/streamrelay"
	"station_signal_api/internal/features/reporting/domain"
)

// record is one physical device shared by every session currently attached to it. A device is
// only ever started on the daemon once, regardless of how many sessions are watching it — see
// Store.Add/Attach/Detach.
type record struct {
	device domain.Device
	hub    *streamrelay.Hub
	// creator is the sessionID whose StartParams actually created this record on the daemon —
	// the one whose params crash re-arm must replay to recreate it (see Snapshot).
	creator  string
	sessions map[string]struct{}
}

// Store is the mutex-guarded in-memory record of active devices, their stream hubs, and the set
// of sessions currently attached to each.
type Store struct {
	mu      sync.RWMutex
	devices map[int]*record
}

// NewStore builds an empty Store.
func NewStore() *Store {
	return &Store{devices: make(map[int]*record)}
}

// Add records a newly started device alongside its stream hub, attached to sessionID as both its
// sole initial session and its creator.
func (s *Store) Add(device domain.Device, hub *streamrelay.Hub, sessionID string) {
	s.mu.Lock()
	s.devices[device.ID] = &record{
		device:   device,
		hub:      hub,
		creator:  sessionID,
		sessions: map[string]struct{}{sessionID: {}},
	}
	s.mu.Unlock()
}

// FindByHostPort returns the active device at host:mmsPort, if any, regardless of which
// session(s) are attached to it.
func (s *Store) FindByHostPort(host string, mmsPort int) (domain.Device, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, r := range s.devices {
		if r.device.Host == host && r.device.MMSPort == mmsPort {
			return r.device, true
		}
	}
	return domain.Device{}, false
}

// Attach adds sessionID to deviceID's attached-session set (idempotent) and returns a copy of
// the device with SessionID stamped to sessionID — never another session's ID, even though the
// underlying record is shared.
func (s *Store) Attach(deviceID int, sessionID string) (domain.Device, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.devices[deviceID]
	if !ok {
		return domain.Device{}, false
	}
	r.sessions[sessionID] = struct{}{}
	device := r.device
	device.SessionID = sessionID
	return device, true
}

// Detach removes sessionID from deviceID's attached-session set. If that was the last attached
// session, the record is dropped and its hub is returned (wasLast=true) for the caller to close.
// Otherwise the record is left alone and (nil, false) is returned.
func (s *Store) Detach(deviceID int, sessionID string) (hub *streamrelay.Hub, wasLast bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.devices[deviceID]
	if !ok {
		return nil, false
	}
	delete(r.sessions, sessionID)
	if len(r.sessions) > 0 {
		return nil, false
	}
	delete(s.devices, deviceID)
	return r.hub, true
}

// IsAttached reports whether sessionID is currently attached to deviceID.
func (s *Store) IsAttached(deviceID int, sessionID string) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	r, ok := s.devices[deviceID]
	if !ok {
		return false
	}
	_, attached := r.sessions[sessionID]
	return attached
}

// AttachedSessionCount returns how many sessions are currently attached to deviceID (0 if it
// doesn't exist).
func (s *Store) AttachedSessionCount(deviceID int) int {
	s.mu.RLock()
	defer s.mu.RUnlock()
	r, ok := s.devices[deviceID]
	if !ok {
		return 0
	}
	return len(r.sessions)
}

// Get returns the device with the given id, if active. Its SessionID field is not authoritative
// once shared across sessions — use IsAttached for ownership checks.
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

// IsConnected reports whether sessionID is currently attached to the device at host:mmsPort —
// regardless of whether other sessions are also attached to it. A device only some other
// session is watching does not count as "connected" for sessionID.
func (s *Store) IsConnected(sessionID, host string, mmsPort int) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, r := range s.devices {
		if r.device.Host == host && r.device.MMSPort == mmsPort {
			_, attached := r.sessions[sessionID]
			return attached
		}
	}
	return false
}

// List returns a snapshot of all currently active devices, one row per attached session (so a
// device shared by two sessions yields two rows, each with SessionID stamped to that session) —
// callers outside this package should use ListForSession instead; this stays for internal use
// (Snapshot goes through the same map, see below).
func (s *Store) List() []domain.Device {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]domain.Device, 0, len(s.devices))
	for _, r := range s.devices {
		// creator first: crash re-arm replays the first row per (host,mmsPort) as a real
		// START_REPORTING, so it must be the session whose StartParams actually created this
		// device — Go's map iteration order over sessions is otherwise unspecified.
		device := r.device
		device.SessionID = r.creator
		out = append(out, device)
		for sessionID := range r.sessions {
			if sessionID == r.creator {
				continue
			}
			device := r.device
			device.SessionID = sessionID
			out = append(out, device)
		}
	}
	return out
}

// ListForSession returns a snapshot of the active devices attached to sessionID.
func (s *Store) ListForSession(sessionID string) []domain.Device {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]domain.Device, 0, len(s.devices))
	for _, r := range s.devices {
		if _, attached := r.sessions[sessionID]; attached {
			device := r.device
			device.SessionID = sessionID
			out = append(out, device)
		}
	}
	return out
}

// Snapshot returns every active device (one row per attached session, full record including its
// owning SessionID), for crash re-arm to replay against a freshly restarted daemon under the
// same sessions that started/attached to them.
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
