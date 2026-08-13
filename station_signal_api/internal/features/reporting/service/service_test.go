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

	stopByAddressDeviceID int
	stopByAddressErr      error

	startCalls int
	gotStopID  int
	stopCalls  int

	stopByAddressCalls   int
	gotStopByAddressHost string
	gotStopByAddressPort int
}

func (m *mockGateway) Start(ctx context.Context, params domain.StartParams) (domain.Device, *streamrelay.Hub, error) {
	m.startCalls++
	return m.startDevice, m.startHub, m.startErr
}

func (m *mockGateway) Stop(ctx context.Context, deviceID int) error {
	m.stopCalls++
	m.gotStopID = deviceID
	return m.stopErr
}

func (m *mockGateway) StopByAddress(ctx context.Context, host string, mmsPort int) (int, error) {
	m.stopByAddressCalls++
	m.gotStopByAddressHost = host
	m.gotStopByAddressPort = mmsPort
	return m.stopByAddressDeviceID, m.stopByAddressErr
}

func newTestHub(t *testing.T) *streamrelay.Hub {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	return streamrelay.NewHub(ctx, "ws://127.0.0.1:1", nil, streamrelay.DefaultBufferSize)
}

const testSessionID = "test-session"

func newServiceWithMock(gw *mockGateway) *Service {
	return &Service{gateway: gw, store: data.NewStore(), locks: newKeyedLocks()}
}

func TestService_Start_Success_StoresDevice(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5"}, startHub: hub}
	svc := newServiceWithMock(gw)

	device, err := svc.Start(context.Background(), testSessionID, domain.StartParams{Host: "10.0.0.5"})

	require.NoError(t, err)
	assert.Equal(t, 1, device.ID)
	assert.Len(t, svc.ListForSession(testSessionID), 1)
	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	cancel()
	_ = ch
}

func TestService_Start_Failure_DoesNotStoreDevice(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrHostAlreadyRunning}
	gw := &mockGateway{startErr: wantErr}
	svc := newServiceWithMock(gw)

	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{Host: "10.0.0.5"})

	assert.Equal(t, wantErr, err)
	assert.Empty(t, svc.ListForSession(testSessionID))
}

func TestService_Stop_Success_RemovesDeviceAndClosesHub(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	defer cancel()

	err = svc.Stop(context.Background(), testSessionID, 1)

	require.NoError(t, err)
	assert.Equal(t, 1, gw.gotStopID)
	assert.Empty(t, svc.ListForSession(testSessionID))
	_, ok = <-ch
	assert.False(t, ok, "hub should be closed after Stop()")
}

func TestService_Stop_Failure_LeavesStoreUntouched(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	gw.stopErr = &daemonproto.Error{Code: daemonproto.ErrOrchestrationFailed}
	err = svc.Stop(context.Background(), testSessionID, 1)

	assert.Equal(t, gw.stopErr, err)
	assert.Len(t, svc.ListForSession(testSessionID), 1, "device should still be tracked since the failure is not DEVICE_NOT_FOUND")
}

func TestService_Stop_DeviceNotFound_StillClearsStore(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	defer cancel()

	gw.stopErr = &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound}
	err = svc.Stop(context.Background(), testSessionID, 1)

	assert.Equal(t, gw.stopErr, err, "the DEVICE_NOT_FOUND error should still be reported to the caller")
	assert.Empty(t, svc.ListForSession(testSessionID), "the daemon has no record of this device either way, so the stale entry should be dropped")
	_, ok = <-ch
	assert.False(t, ok, "hub should be closed even though the daemon call failed")
}

func TestService_Stop_OtherSessionsDeviceIsRejectedAsNotFound(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	err = svc.Stop(context.Background(), "someone-elses-session", 1)

	var derr *daemonproto.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, daemonproto.ErrDeviceNotFound, derr.Code)
	assert.Zero(t, gw.gotStopID, "the daemon should never be asked to stop a device this session doesn't own")
	assert.Len(t, svc.ListForSession(testSessionID), 1, "the original session's device must be untouched")
}

func TestService_StopByAddress_NothingTracked_CallsGatewayAndSucceeds(t *testing.T) {
	gw := &mockGateway{stopByAddressDeviceID: 7}
	svc := newServiceWithMock(gw)

	err := svc.StopByAddress(context.Background(), "10.0.0.9", 102)

	require.NoError(t, err)
	assert.Equal(t, 1, gw.stopByAddressCalls)
	assert.Equal(t, "10.0.0.9", gw.gotStopByAddressHost)
	assert.Equal(t, 102, gw.gotStopByAddressPort)
}

func TestService_StopByAddress_DeviceNotFoundFromDaemon_TreatedAsSuccess(t *testing.T) {
	gw := &mockGateway{stopByAddressErr: &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound}}
	svc := newServiceWithMock(gw)

	err := svc.StopByAddress(context.Background(), "10.0.0.9", 102)

	assert.NoError(t, err, "nothing registered at this address is an idempotent success for this recovery-focused call")
}

func TestService_StopByAddress_OtherErrorFromDaemon_Propagates(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrStartInProgress}
	gw := &mockGateway{stopByAddressErr: wantErr}
	svc := newServiceWithMock(gw)

	err := svc.StopByAddress(context.Background(), "10.0.0.9", 102)

	assert.Equal(t, wantErr, err)
}

func TestService_StopByAddress_AlreadyTracked_RefusesWithoutCallingDaemon(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.9", MMSPort: 102}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{Host: "10.0.0.9", MMSPort: 102})
	require.NoError(t, err)

	err = svc.StopByAddress(context.Background(), "10.0.0.9", 102)

	var derr *daemonproto.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, daemonproto.ErrDeviceTracked, derr.Code)
	assert.Zero(t, gw.stopByAddressCalls, "should never reach the daemon when the API already has a record for this address")
}

func TestService_ListForSession_ReflectsOwnActiveDevices(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	require.Empty(t, svc.ListForSession(testSessionID))

	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)

	assert.Len(t, svc.ListForSession(testSessionID), 1)
	assert.Empty(t, svc.ListForSession("someone-elses-session"))
}

func TestService_OwnsDevice(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)

	assert.True(t, svc.OwnsDevice(testSessionID, 1))
	assert.False(t, svc.OwnsDevice("someone-elses-session", 1))
	assert.False(t, svc.OwnsDevice(testSessionID, 999))
}

func TestService_Snapshot_ReturnsDevicesWithSessionAndStartParams(t *testing.T) {
	params := domain.StartParams{Host: "10.0.0.5", InterfaceID: "eth0"}
	gw := &mockGateway{startDevice: domain.Device{ID: 1, StartParams: params}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), testSessionID, params)
	require.NoError(t, err)

	got := svc.Snapshot()

	require.Len(t, got, 1)
	assert.Equal(t, params, got[0].StartParams)
	assert.Equal(t, testSessionID, got[0].SessionID)
}

func TestService_Clear_EmptiesStoreAndClosesHubs(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{})
	require.NoError(t, err)
	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	defer cancel()

	svc.Clear()

	assert.Empty(t, svc.ListForSession(testSessionID))
	_, ok = <-ch
	assert.False(t, ok)
}

func TestService_IsConnected_ReflectsActiveDevices(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	assert.False(t, svc.IsConnected(testSessionID, "10.0.0.5", 102))

	_, err := svc.Start(context.Background(), testSessionID, domain.StartParams{Host: "10.0.0.5"})
	require.NoError(t, err)

	assert.True(t, svc.IsConnected(testSessionID, "10.0.0.5", 102))
	assert.False(t, svc.IsConnected(testSessionID, "10.0.0.6", 102), "different host should not match")
}

func TestService_IsConnected_FalseForASessionThatIsNotAttached(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), "session-a", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})
	require.NoError(t, err)

	assert.False(t, svc.IsConnected("session-b", "10.0.0.5", 102),
		"session-a being connected must not make the device look connected for session-b's own scan filter")

	_, err = svc.Start(context.Background(), "session-b", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})
	require.NoError(t, err)

	assert.True(t, svc.IsConnected("session-b", "10.0.0.5", 102), "true once session-b attaches to the shared device itself")
}

func TestService_StreamFor_UnknownDeviceReturnsFalse(t *testing.T) {
	svc := newServiceWithMock(&mockGateway{})

	ch, cancel, ok := svc.StreamFor(999)

	assert.False(t, ok)
	assert.Nil(t, ch)
	assert.Nil(t, cancel)
}

func TestService_Start_SecondSessionSameHostPort_AttachesWithoutCallingDaemon(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	first, err := svc.Start(context.Background(), "session-a", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})
	require.NoError(t, err)

	second, err := svc.Start(context.Background(), "session-b", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})

	require.NoError(t, err)
	assert.Equal(t, 1, gw.startCalls, "the daemon must only be asked to START_REPORTING once for a shared device")
	assert.Equal(t, first.ID, second.ID, "both sessions attach to the same underlying device")
	assert.True(t, svc.OwnsDevice("session-a", 1))
	assert.True(t, svc.OwnsDevice("session-b", 1))
	assert.Len(t, svc.ListForSession("session-a"), 1)
	assert.Len(t, svc.ListForSession("session-b"), 1)
}

func TestService_Start_DifferentHostPort_CallsDaemonAgain(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), "session-a", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})
	require.NoError(t, err)

	gw.startDevice = domain.Device{ID: 2, Host: "10.0.0.6", MMSPort: 102}
	_, err = svc.Start(context.Background(), "session-b", domain.StartParams{Host: "10.0.0.6", MMSPort: 102})

	require.NoError(t, err)
	assert.Equal(t, 2, gw.startCalls, "a different physical device must still get its own START_REPORTING call")
}

func TestService_Stop_NonLastSession_DetachesWithoutCallingDaemon(t *testing.T) {
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, startHub: newTestHub(t)}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), "session-a", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})
	require.NoError(t, err)
	_, err = svc.Start(context.Background(), "session-b", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})
	require.NoError(t, err)

	err = svc.Stop(context.Background(), "session-a", 1)

	require.NoError(t, err)
	assert.Zero(t, gw.stopCalls, "STOP_REPORTING must not be issued while another session is still attached")
	assert.False(t, svc.OwnsDevice("session-a", 1))
	assert.True(t, svc.OwnsDevice("session-b", 1), "the remaining session's attachment must survive")
}

func TestService_Stop_LastSession_CallsDaemonAndClosesHub(t *testing.T) {
	hub := newTestHub(t)
	gw := &mockGateway{startDevice: domain.Device{ID: 1, Host: "10.0.0.5", MMSPort: 102}, startHub: hub}
	svc := newServiceWithMock(gw)
	_, err := svc.Start(context.Background(), "session-a", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})
	require.NoError(t, err)
	_, err = svc.Start(context.Background(), "session-b", domain.StartParams{Host: "10.0.0.5", MMSPort: 102})
	require.NoError(t, err)

	require.NoError(t, svc.Stop(context.Background(), "session-a", 1))
	assert.Zero(t, gw.stopCalls, "still one session left, so the daemon must not be called yet")

	ch, cancel, ok := svc.StreamFor(1)
	require.True(t, ok)
	defer cancel()

	err = svc.Stop(context.Background(), "session-b", 1)

	require.NoError(t, err)
	assert.Equal(t, 1, gw.stopCalls, "the last session's Stop must actually tear the device down")
	assert.Equal(t, 1, gw.gotStopID)
	_, ok = <-ch
	assert.False(t, ok, "hub should be closed once the last session detaches")
}
