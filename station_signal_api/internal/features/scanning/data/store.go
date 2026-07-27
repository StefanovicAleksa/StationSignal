// Package data is scanning's I/O layer: the mutex-guarded active-scan store (which also owns
// the shared scan-result hub's 0<->1 lifecycle) and the daemon-facing gateway calls. Imports
// domain for types; never imported by domain.
package data

import (
	"context"
	"log/slog"
	"sync"

	"station_signal_api/internal/core/streamrelay"
	"station_signal_api/internal/features/scanning/domain"
)

const scanStreamURL = "ws://127.0.0.1:8766"

// Store is the mutex-guarded in-memory record of active scans and the singleton scan-result
// hub shared by all of them.
type Store struct {
	hubCtx context.Context
	logger *slog.Logger

	mu    sync.RWMutex
	scans map[int]domain.Scan
	hub   *streamrelay.Hub
}

// NewStore builds an empty Store. hubCtx is the parent context for the scan hub's upstream
// connection goroutine — it should live for the whole process lifetime and only be canceled
// at shutdown.
func NewStore(hubCtx context.Context, logger *slog.Logger) *Store {
	return &Store{hubCtx: hubCtx, logger: logger, scans: make(map[int]domain.Scan)}
}

// Add records a newly started scan, opening the shared scan hub on the 0->1 transition.
func (s *Store) Add(scan domain.Scan) {
	s.mu.Lock()
	if len(s.scans) == 0 && s.hub == nil {
		s.hub = streamrelay.NewHub(s.hubCtx, scanStreamURL, s.logger)
	}
	s.scans[scan.ID] = scan
	s.mu.Unlock()
}

// Remove drops a scan, closing the shared scan hub on the 1->0 transition.
func (s *Store) Remove(id int) {
	s.mu.Lock()
	delete(s.scans, id)
	var hubToClose *streamrelay.Hub
	if len(s.scans) == 0 && s.hub != nil {
		hubToClose = s.hub
		s.hub = nil
	}
	s.mu.Unlock()

	if hubToClose != nil {
		hubToClose.Close()
	}
}

// Get returns the scan with the given id, if active.
func (s *Store) Get(id int) (domain.Scan, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	sc, ok := s.scans[id]
	return sc, ok
}

// Hub returns the shared scan-result hub, if at least one scan is active.
func (s *Store) Hub() (*streamrelay.Hub, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.hub, s.hub != nil
}

// List returns a snapshot of all currently active scans, regardless of owning session —
// callers outside this package should use ListForSession instead; this stays for internal use
// (crash re-arm's Snapshot goes through the same map, see below).
func (s *Store) List() []domain.Scan {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]domain.Scan, 0, len(s.scans))
	for _, sc := range s.scans {
		out = append(out, sc)
	}
	return out
}

// ListForSession returns a snapshot of the active scans owned by sessionID.
func (s *Store) ListForSession(sessionID string) []domain.Scan {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]domain.Scan, 0, len(s.scans))
	for _, sc := range s.scans {
		if sc.SessionID == sessionID {
			out = append(out, sc)
		}
	}
	return out
}

// Snapshot returns every active scan (full record, including its owning SessionID), for crash
// re-arm to replay against a freshly restarted daemon under the same sessions that started them.
func (s *Store) Snapshot() []domain.Scan {
	return s.List()
}

// Clear drops every scan and closes the shared hub — used during crash re-arm, where the old
// hub is already dead (its upstream connection died with the old daemon process) and gets
// replaced wholesale.
func (s *Store) Clear() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.hub != nil {
		s.hub.Close()
	}
	s.scans = make(map[int]domain.Scan)
	s.hub = nil
}
