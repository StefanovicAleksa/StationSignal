package data

import (
	"context"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/streamrelay"
	"station_signal_api/internal/features/reporting/domain"
)

// newTestHub builds a real Hub against a URL nothing is listening on. Its background dial
// fails harmlessly and Add()/Detach()/Clear() are synchronous w.r.t. the store's map state
// regardless of dial outcome — see streamrelay's own tests for the dial-failure behavior.
func newTestHub(t *testing.T) *streamrelay.Hub {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	return streamrelay.NewHub(ctx, "ws://127.0.0.1:1", nil)
}

func TestStore_AddAndGet(t *testing.T) {
	s := NewStore()
	device := domain.Device{ID: 1, Host: "10.0.0.5"}
	hub := newTestHub(t)

	s.Add(device, hub, "session-a")

	got, ok := s.Get(1)
	require.True(t, ok)
	assert.Equal(t, device, got)
}

func TestStore_GetUnknownReturnsFalse(t *testing.T) {
	s := NewStore()

	_, ok := s.Get(999)

	assert.False(t, ok)
}

func TestStore_Hub(t *testing.T) {
	s := NewStore()
	hub := newTestHub(t)
	s.Add(domain.Device{ID: 1}, hub, "session-a")

	got, ok := s.Hub(1)

	require.True(t, ok)
	assert.Same(t, hub, got)
}

func TestStore_HubUnknownReturnsFalse(t *testing.T) {
	s := NewStore()

	_, ok := s.Hub(999)

	assert.False(t, ok)
}

func TestStore_DetachLastSessionReturnsHubAndDropsEntry(t *testing.T) {
	s := NewStore()
	hub := newTestHub(t)
	s.Add(domain.Device{ID: 1}, hub, "session-a")

	got, wasLast := s.Detach(1, "session-a")
	require.True(t, wasLast)
	assert.Same(t, hub, got)

	_, ok := s.Get(1)
	assert.False(t, ok)
}

func TestStore_DetachUnknownReturnsFalse(t *testing.T) {
	s := NewStore()

	hub, wasLast := s.Detach(999, "session-a")

	assert.False(t, wasLast)
	assert.Nil(t, hub)
}

func TestStore_DetachNotLastSessionKeepsRecordAlive(t *testing.T) {
	s := NewStore()
	hub := newTestHub(t)
	s.Add(domain.Device{ID: 1}, hub, "session-a")
	s.Attach(1, "session-b")

	got, wasLast := s.Detach(1, "session-a")

	assert.False(t, wasLast)
	assert.Nil(t, got)
	_, ok := s.Get(1)
	assert.True(t, ok, "record should survive while session-b is still attached")
	assert.True(t, s.IsAttached(1, "session-b"))
	assert.False(t, s.IsAttached(1, "session-a"))
}

func TestStore_Attach_AddsSessionToExistingRecord(t *testing.T) {
	s := NewStore()
	device := domain.Device{ID: 1, Host: "10.0.0.5"}
	s.Add(device, newTestHub(t), "session-a")

	got, ok := s.Attach(1, "session-b")

	require.True(t, ok)
	assert.Equal(t, "session-b", got.SessionID, "attached copy must be stamped with the attaching session, not the creator's")
	assert.Equal(t, 2, s.AttachedSessionCount(1))
	assert.True(t, s.IsAttached(1, "session-a"))
	assert.True(t, s.IsAttached(1, "session-b"))
}

func TestStore_Attach_UnknownDeviceReturnsFalse(t *testing.T) {
	s := NewStore()

	_, ok := s.Attach(999, "session-a")

	assert.False(t, ok)
}

func TestStore_Attach_SameSessionTwiceIsIdempotent(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1}, newTestHub(t), "session-a")

	_, ok := s.Attach(1, "session-a")

	require.True(t, ok)
	assert.Equal(t, 1, s.AttachedSessionCount(1))
}

func TestStore_IsAttached_UnknownDeviceReturnsFalse(t *testing.T) {
	s := NewStore()

	assert.False(t, s.IsAttached(999, "session-a"))
}

func TestStore_AttachedSessionCount_UnknownDeviceReturnsZero(t *testing.T) {
	s := NewStore()

	assert.Zero(t, s.AttachedSessionCount(999))
}

func TestStore_FindByHostPort(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, newTestHub(t), "session-a")

	got, ok := s.FindByHostPort("10.0.0.5", 102)
	require.True(t, ok)
	assert.Equal(t, 1, got.ID)

	_, ok = s.FindByHostPort("10.0.0.5", 103)
	assert.False(t, ok, "different port should not match")
	_, ok = s.FindByHostPort("10.0.0.6", 102)
	assert.False(t, ok, "different host should not match")
}

func TestStore_List(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1}, newTestHub(t), "session-a")
	s.Add(domain.Device{ID: 2}, newTestHub(t), "session-b")

	got := s.List()

	assert.Len(t, got, 2)
}

func TestStore_List_OneRowPerAttachedSession(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1}, newTestHub(t), "session-a")
	s.Attach(1, "session-b")
	s.Attach(1, "session-c")

	got := s.List()

	require.Len(t, got, 3)
	sessionIDs := make(map[string]bool)
	for _, d := range got {
		sessionIDs[d.SessionID] = true
	}
	assert.Equal(t, map[string]bool{"session-a": true, "session-b": true, "session-c": true}, sessionIDs)
}

func TestStore_ListEmptyReturnsEmptySliceNotNil(t *testing.T) {
	s := NewStore()

	got := s.List()

	assert.NotNil(t, got)
	assert.Empty(t, got)
}

func TestStore_ListForSession_OnlyReturnsMatchingSession(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1}, newTestHub(t), "session-a")
	s.Add(domain.Device{ID: 2}, newTestHub(t), "session-b")

	got := s.ListForSession("session-a")

	require.Len(t, got, 1)
	assert.Equal(t, 1, got[0].ID)
	assert.Equal(t, "session-a", got[0].SessionID)
}

func TestStore_ListForSession_SharedDeviceAppearsForEveryAttachedSession(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1}, newTestHub(t), "session-a")
	s.Attach(1, "session-b")

	require.Len(t, s.ListForSession("session-a"), 1)
	require.Len(t, s.ListForSession("session-b"), 1)
	assert.Empty(t, s.ListForSession("session-c"))
}

func TestStore_Snapshot(t *testing.T) {
	s := NewStore()
	params := domain.StartParams{Host: "10.0.0.5", InterfaceID: "eth0"}
	s.Add(domain.Device{ID: 1, StartParams: params}, newTestHub(t), "session-a")

	got := s.Snapshot()

	require.Len(t, got, 1)
	assert.Equal(t, params, got[0].StartParams)
	assert.Equal(t, "session-a", got[0].SessionID)
}

func TestStore_Snapshot_EmitsCreatorRowFirst(t *testing.T) {
	s := NewStore()
	params := domain.StartParams{Host: "10.0.0.5", InterfaceID: "eth0", AccessMode: domain.AccessModeReadOnly}
	s.Add(domain.Device{ID: 1, StartParams: params}, newTestHub(t), "creator-session")
	s.Attach(1, "attacher-session")
	s.Attach(1, "another-attacher")

	got := s.Snapshot()

	require.Len(t, got, 3)
	assert.Equal(t, "creator-session", got[0].SessionID, "creator's row (whose StartParams crash re-arm actually replays to the daemon) must come first")
}

func TestStore_ClearClosesHubsAndEmptiesStore(t *testing.T) {
	s := NewStore()
	hub := newTestHub(t)
	s.Add(domain.Device{ID: 1}, hub, "session-a")
	// Subscribe before Clear() so we have a channel that must observe the hub closing.
	ch, cancel := hub.Subscribe()
	defer cancel()

	s.Clear()

	assert.Empty(t, s.List())
	_, ok := <-ch
	assert.False(t, ok, "hub should have been closed by Clear()")
}

func TestStore_IsConnected_MatchesHostAndPortForAttachedSession(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, newTestHub(t), "session-a")

	assert.True(t, s.IsConnected("session-a", "10.0.0.5", 102))
	assert.False(t, s.IsConnected("session-a", "10.0.0.5", 103), "different port should not match")
	assert.False(t, s.IsConnected("session-a", "10.0.0.6", 102), "different host should not match")
}

func TestStore_IsConnected_FalseForASessionNotAttached(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, newTestHub(t), "session-a")

	assert.False(t, s.IsConnected("session-b", "10.0.0.5", 102),
		"another session watching the same device must not count as this session being connected")

	s.Attach(1, "session-b")

	assert.True(t, s.IsConnected("session-b", "10.0.0.5", 102), "true once session-b actually attaches")
}

func TestStore_IsConnected_EmptyStoreReturnsFalse(t *testing.T) {
	s := NewStore()

	assert.False(t, s.IsConnected("session-a", "10.0.0.5", 102))
}

func TestStore_ClearOnEmptyStoreDoesNotPanic(t *testing.T) {
	s := NewStore()

	assert.NotPanics(t, s.Clear)
}
