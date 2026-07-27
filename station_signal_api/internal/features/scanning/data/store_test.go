package data

import (
	"context"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/features/scanning/domain"
)

func newTestStore(t *testing.T) *Store {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	return NewStore(ctx, nil)
}

func TestStore_AddAndGet(t *testing.T) {
	s := newTestStore(t)
	scan := domain.Scan{ID: 1, InterfaceID: "eth0"}

	s.Add(scan)

	got, ok := s.Get(1)
	require.True(t, ok)
	assert.Equal(t, scan, got)
}

func TestStore_GetUnknownReturnsFalse(t *testing.T) {
	s := newTestStore(t)

	_, ok := s.Get(999)

	assert.False(t, ok)
}

func TestStore_HubAbsentWhenNoScansActive(t *testing.T) {
	s := newTestStore(t)

	_, ok := s.Hub()

	assert.False(t, ok)
}

func TestStore_Add_CreatesHubOnZeroToOneTransition(t *testing.T) {
	s := newTestStore(t)

	s.Add(domain.Scan{ID: 1})

	hub, ok := s.Hub()
	require.True(t, ok)
	assert.NotNil(t, hub)
}

func TestStore_Add_SecondScanReusesSameHub(t *testing.T) {
	s := newTestStore(t)
	s.Add(domain.Scan{ID: 1})
	first, ok := s.Hub()
	require.True(t, ok)

	s.Add(domain.Scan{ID: 2})
	second, ok := s.Hub()
	require.True(t, ok)

	assert.Same(t, first, second)
}

func TestStore_Remove_OneToZeroTransitionClosesHub(t *testing.T) {
	s := newTestStore(t)
	s.Add(domain.Scan{ID: 1})
	hub, ok := s.Hub()
	require.True(t, ok)
	ch, cancel := hub.Subscribe()
	defer cancel()

	s.Remove(1)

	_, ok = <-ch
	assert.False(t, ok, "hub should be closed once the last scan is removed")
	_, ok = s.Hub()
	assert.False(t, ok)
}

func TestStore_Remove_StillActiveScansKeepHubAlive(t *testing.T) {
	s := newTestStore(t)
	s.Add(domain.Scan{ID: 1})
	s.Add(domain.Scan{ID: 2})
	hubBefore, _ := s.Hub()

	s.Remove(1)

	hubAfter, ok := s.Hub()
	require.True(t, ok)
	assert.Same(t, hubBefore, hubAfter)
}

func TestStore_RemoveUnknownIsSafe(t *testing.T) {
	s := newTestStore(t)

	assert.NotPanics(t, func() { s.Remove(999) })
}

func TestStore_List(t *testing.T) {
	s := newTestStore(t)
	s.Add(domain.Scan{ID: 1})
	s.Add(domain.Scan{ID: 2})

	assert.Len(t, s.List(), 2)
}

func TestStore_ListEmptyReturnsEmptySliceNotNil(t *testing.T) {
	s := newTestStore(t)

	got := s.List()

	assert.NotNil(t, got)
	assert.Empty(t, got)
}

func TestStore_ListForSession_OnlyReturnsMatchingSession(t *testing.T) {
	s := newTestStore(t)
	s.Add(domain.Scan{ID: 1, SessionID: "session-a"})
	s.Add(domain.Scan{ID: 2, SessionID: "session-b"})

	got := s.ListForSession("session-a")

	require.Len(t, got, 1)
	assert.Equal(t, 1, got[0].ID)
}

func TestStore_Snapshot(t *testing.T) {
	s := newTestStore(t)
	params := domain.StartParams{InterfaceID: "eth0"}
	s.Add(domain.Scan{ID: 1, StartParams: params, SessionID: "session-a"})

	got := s.Snapshot()

	require.Len(t, got, 1)
	assert.Equal(t, params, got[0].StartParams)
	assert.Equal(t, "session-a", got[0].SessionID)
}

func TestStore_ClearClosesHubAndEmptiesStore(t *testing.T) {
	s := newTestStore(t)
	s.Add(domain.Scan{ID: 1})
	hub, _ := s.Hub()
	ch, cancel := hub.Subscribe()
	defer cancel()

	s.Clear()

	assert.Empty(t, s.List())
	_, ok := s.Hub()
	assert.False(t, ok)
	_, ok = <-ch
	assert.False(t, ok)
}

func TestStore_ClearOnEmptyStoreDoesNotPanic(t *testing.T) {
	s := newTestStore(t)

	assert.NotPanics(t, s.Clear)
}
