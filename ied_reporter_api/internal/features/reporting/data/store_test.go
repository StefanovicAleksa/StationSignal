package data

import (
	"context"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"ied_reporter_api/internal/core/streamrelay"
	"ied_reporter_api/internal/features/reporting/domain"
)

// newTestHub builds a real Hub against a URL nothing is listening on. Its background dial
// fails harmlessly and Add()/Remove()/Clear() are synchronous w.r.t. the store's map state
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

	s.Add(device, hub)

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
	s.Add(domain.Device{ID: 1}, hub)

	got, ok := s.Hub(1)

	require.True(t, ok)
	assert.Same(t, hub, got)
}

func TestStore_HubUnknownReturnsFalse(t *testing.T) {
	s := NewStore()

	_, ok := s.Hub(999)

	assert.False(t, ok)
}

func TestStore_RemoveReturnsHubAndDropsEntry(t *testing.T) {
	s := NewStore()
	hub := newTestHub(t)
	s.Add(domain.Device{ID: 1}, hub)

	got, ok := s.Remove(1)
	require.True(t, ok)
	assert.Same(t, hub, got)

	_, ok = s.Get(1)
	assert.False(t, ok)
}

func TestStore_RemoveUnknownReturnsFalse(t *testing.T) {
	s := NewStore()

	hub, ok := s.Remove(999)

	assert.False(t, ok)
	assert.Nil(t, hub)
}

func TestStore_List(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1}, newTestHub(t))
	s.Add(domain.Device{ID: 2}, newTestHub(t))

	got := s.List()

	assert.Len(t, got, 2)
}

func TestStore_ListEmptyReturnsEmptySliceNotNil(t *testing.T) {
	s := NewStore()

	got := s.List()

	assert.NotNil(t, got)
	assert.Empty(t, got)
}

func TestStore_Snapshot(t *testing.T) {
	s := NewStore()
	params := domain.StartParams{Host: "10.0.0.5", InterfaceID: "eth0"}
	s.Add(domain.Device{ID: 1, StartParams: params}, newTestHub(t))

	got := s.Snapshot()

	require.Len(t, got, 1)
	assert.Equal(t, params, got[0])
}

func TestStore_ClearClosesHubsAndEmptiesStore(t *testing.T) {
	s := NewStore()
	hub := newTestHub(t)
	s.Add(domain.Device{ID: 1}, hub)
	// Subscribe before Clear() so we have a channel that must observe the hub closing.
	ch, cancel := hub.Subscribe()
	defer cancel()

	s.Clear()

	assert.Empty(t, s.List())
	_, ok := <-ch
	assert.False(t, ok, "hub should have been closed by Clear()")
}

func TestStore_IsConnected_MatchesHostAndPort(t *testing.T) {
	s := NewStore()
	s.Add(domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, newTestHub(t))

	assert.True(t, s.IsConnected("10.0.0.5", 102))
	assert.False(t, s.IsConnected("10.0.0.5", 103), "different port should not match")
	assert.False(t, s.IsConnected("10.0.0.6", 102), "different host should not match")
}

func TestStore_IsConnected_EmptyStoreReturnsFalse(t *testing.T) {
	s := NewStore()

	assert.False(t, s.IsConnected("10.0.0.5", 102))
}

func TestStore_ClearOnEmptyStoreDoesNotPanic(t *testing.T) {
	s := NewStore()

	assert.NotPanics(t, s.Clear)
}
