package service

import (
	"context"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/daemonproto"
	"station_signal_api/internal/features/scanning/data"
	"station_signal_api/internal/features/scanning/domain"
)

type mockGateway struct {
	startScan domain.Scan
	startErr  error

	stopErr error

	gotStopID int
}

const testSessionID = "test-session"

func (m *mockGateway) Start(ctx context.Context, params domain.StartParams) (domain.Scan, error) {
	return m.startScan, m.startErr
}

func (m *mockGateway) Stop(ctx context.Context, scanID int) error {
	m.gotStopID = scanID
	return m.stopErr
}

func newServiceWithMock(t *testing.T, gw *mockGateway) *Service {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	return &Service{gateway: gw, store: data.NewStore(ctx, nil)}
}

func TestService_Start_Success_StoresScan(t *testing.T) {
	gw := &mockGateway{startScan: domain.Scan{ID: 1, InterfaceID: "eth0"}}
	svc := newServiceWithMock(t, gw)

	scan, err := svc.Start(context.Background(), testSessionID, domain.StartParams{InterfaceID: "eth0"})

	require.NoError(t, err)
	assert.Equal(t, 1, scan.ID)
	assert.Len(t, svc.ListForSession(testSessionID), 1)
}

func TestService_Start_Failure_DoesNotStoreScan(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrDispatcherStartFailed}
	gw := &mockGateway{startErr: wantErr}
	svc := newServiceWithMock(t, gw)

	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{InterfaceID: "eth0"})

	assert.Equal(t, wantErr, err)
	assert.Empty(t, svc.ListForSession(testSessionID))
}

func TestService_Stop_Success_RemovesScan(t *testing.T) {
	gw := &mockGateway{startScan: domain.Scan{ID: 1}}
	svc := newServiceWithMock(t, gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)

	err = svc.Stop(context.Background(), testSessionID, 1)

	require.NoError(t, err)
	assert.Equal(t, 1, gw.gotStopID)
	assert.Empty(t, svc.ListForSession(testSessionID))
}

func TestService_Stop_Failure_LeavesStoreUntouched(t *testing.T) {
	gw := &mockGateway{startScan: domain.Scan{ID: 1}}
	svc := newServiceWithMock(t, gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)

	gw.stopErr = &daemonproto.Error{Code: daemonproto.ErrScanNotFound}
	err = svc.Stop(context.Background(), testSessionID, 1)

	assert.Equal(t, gw.stopErr, err)
	assert.Len(t, svc.ListForSession(testSessionID), 1)
}

func TestService_Stop_OtherSessionsScanIsRejectedAsNotFound(t *testing.T) {
	gw := &mockGateway{startScan: domain.Scan{ID: 1}}
	svc := newServiceWithMock(t, gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)

	err = svc.Stop(context.Background(), "someone-elses-session", 1)

	var derr *daemonproto.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, daemonproto.ErrScanNotFound, derr.Code)
	assert.Zero(t, gw.gotStopID, "the daemon should never be asked to stop a scan this session doesn't own")
	assert.Len(t, svc.ListForSession(testSessionID), 1, "the original session's scan must be untouched")
}

func TestService_ListForSession_OnlyReturnsOwnScans(t *testing.T) {
	gw := &mockGateway{startScan: domain.Scan{ID: 1}}
	svc := newServiceWithMock(t, gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)

	assert.Empty(t, svc.ListForSession("someone-elses-session"))
	assert.Len(t, svc.ListForSession(testSessionID), 1)
}

func TestService_OwnsScan(t *testing.T) {
	gw := &mockGateway{startScan: domain.Scan{ID: 1}}
	svc := newServiceWithMock(t, gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)

	assert.True(t, svc.OwnsScan(testSessionID, 1))
	assert.False(t, svc.OwnsScan("someone-elses-session", 1))
	assert.False(t, svc.OwnsScan(testSessionID, 999))
}

func TestService_Snapshot_ReturnsScansWithSessionAndStartParams(t *testing.T) {
	params := domain.StartParams{InterfaceID: "eth0"}
	gw := &mockGateway{startScan: domain.Scan{ID: 1, StartParams: params}}
	svc := newServiceWithMock(t, gw)
	_, err := svc.Start(context.Background(), testSessionID, params)
	require.NoError(t, err)

	got := svc.Snapshot()

	require.Len(t, got, 1)
	assert.Equal(t, params, got[0].StartParams)
	assert.Equal(t, testSessionID, got[0].SessionID)
}

func TestService_Clear_EmptiesStoreAndClosesSharedHub(t *testing.T) {
	gw := &mockGateway{startScan: domain.Scan{ID: 1}}
	svc := newServiceWithMock(t, gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)
	ch, cancel, ok := svc.StreamScans()
	require.True(t, ok)
	defer cancel()

	svc.Clear()

	assert.Empty(t, svc.ListForSession(testSessionID))
	_, ok = <-ch
	assert.False(t, ok)
}

func TestService_StreamScans_NoActiveScanReturnsFalse(t *testing.T) {
	svc := newServiceWithMock(t, &mockGateway{})

	ch, cancel, ok := svc.StreamScans()

	assert.False(t, ok)
	assert.Nil(t, ch)
	assert.Nil(t, cancel)
}
