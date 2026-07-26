package service

import (
	"context"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/daemonproto"
	"station_signal_api/internal/core/streamrelay"
	"station_signal_api/internal/features/reporting/data"
	"station_signal_api/internal/features/reporting/domain"
)

type mockGateway struct {
	startDevice domain.Device
	startHub    *streamrelay.Hub
	startErr    error

	stopErr error

	gotStopID int
}

func (m *mockGateway) Start(ctx context.Context, params domain.StartParams) (domain.Device, *streamrelay.Hub, error) {
	return m.startDevice, m.startHub, m.startErr
}

func (m *mockGateway) Stop(ctx context.Context, deviceID int) error {
	m.gotStopID = deviceID
	return m.stopErr
}

func newTestHub(t *testing.T) *streamrelay.Hub {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	return streamrelay.NewHub(ctx, "ws://127.0.0.1:1", nil)
}

func newServiceWithMock(gw *mockGateway) *Service {
	return &Service{gateway: gw, store: data.NewStore()}
}

func TestService_Start_Success_StoresDevice(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5"}, startHub: hub}
	svc := newServiceWithMock(gw)

	device, err := svc.Start(context.Background(), domain.StartParams{Host: "10.0.0.5"})

	require.NoError(t, err)
	assert.Equal(t, 1, device.ID)
	assert.Len(t, svc.List(), 1)
	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	cancel()
	_ = ch
}

func TestService_Start_Failure_DoesNotStoreDevice(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrHostAlreadyRunning}
	gw := &mockGateway{startErr: wantErr}
	svc := newServiceWithMock(gw)

	_, err := svc.Start(context.Background(), domain.StartParams{Host: "10.0.0.5"})

	assert.Equal(t, wantErr, err)
	assert.Empty(t, svc.List())
}

func TestService_Stop_Success_RemovesDeviceAndClosesHub(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	defer cancel()

	err = svc.Stop(context.Background(), 1)

	require.NoError(t, err)
	assert.Equal(t, 1, gw.gotStopID)
	assert.Empty(t, svc.List())
	_, ok = <-ch
	assert.False(t, ok, "hub should be closed after Stop()")
}

func TestService_Stop_Failure_LeavesStoreUntouched(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	gw.stopErr = &daemonproto.Error{Code: daemonproto.ErrOrchestrationFailed}
	err = svc.Stop(context.Background(), 1)

	assert.Equal(t, gw.stopErr, err)
	assert.Len(t, svc.List(), 1, "device should still be tracked since the failure is not DEVICE_NOT_FOUND")
}

func TestService_Stop_DeviceNotFound_StillClearsStore(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	defer cancel()

	gw.stopErr = &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound}
	err = svc.Stop(context.Background(), 1)

	assert.Equal(t, gw.stopErr, err, "the DEVICE_NOT_FOUND error should still be reported to the caller")
	assert.Empty(t, svc.List(), "the daemon has no record of this device either way, so the stale entry should be dropped")
	_, ok = <-ch
	assert.False(t, ok, "hub should be closed even though the daemon call failed")
}

func TestService_List_ReflectsActiveDevices(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	require.Empty(t, svc.List())

	_, err := svc.Start(context.Background(), domain.StartParams{})
	require.NoError(t, err)

	assert.Len(t, svc.List(), 1)
}

func TestService_Snapshot_ReturnsOriginalStartParams(t *testing.T) {
	params := domain.StartParams{Host: "10.0.0.5", InterfaceID: "eth0"}
	gw := &mockGateway{startDevice: domain.Device{ID: 1, StartParams: params}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), params)
	require.NoError(t, err)

	got := svc.Snapshot()

	require.Len(t, got, 1)
	assert.Equal(t, params, got[0])
}

func TestService_Clear_EmptiesStoreAndClosesHubs(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), domain.StartParams{})
	require.NoError(t, err)
	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	defer cancel()

	svc.Clear()

	assert.Empty(t, svc.List())
	_, ok = <-ch
	assert.False(t, ok)
}

func TestService_IsConnected_ReflectsActiveDevices(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	assert.False(t, svc.IsConnected("10.0.0.5", 102))

	_, err := svc.Start(context.Background(), domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	assert.True(t, svc.IsConnected("10.0.0.5", 102))
	assert.False(t, svc.IsConnected("10.0.0.6", 102), "different host should not match")
}

func TestService_StreamFor_UnknownDeviceReturnsFalse(t *testing.T) {
	svc := newServiceWithMock(&mockGateway{})

	ch, cancel, ok := svc.StreamFor(999)

	assert.False(t, ok)
	assert.Nil(t, ch)
	assert.Nil(t, cancel)
}
