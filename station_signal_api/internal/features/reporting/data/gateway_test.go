package data

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/daemonproto"
	"station_signal_api/internal/features/reporting/domain"
)

type mockCaller struct {
	gotAction string
	gotParams any
	result    json.RawMessage
	err       error
}

func (m *mockCaller) Call(ctx context.Context, action string, params any) (json.RawMessage, error) {
	m.gotAction = action
	m.gotParams = params
	return m.result, m.err
}

func TestGateway_Start_MapsParamsAndResult(t *testing.T) {
	caller := &mockCaller{result: json.RawMessage(`{"deviceId":7,"wsPort":9007}`)}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	gw := NewGateway(caller, ctx, nil)

	params := domain.StartParams{
		Host:         "10.0.0.5",
		InterfaceID:  "eth0",
		AccessMode:   domain.AccessModeReadOnly,
		LnCategories: []string{"CONTROL", "OTHER"},
	}
	device, hub, err := gw.Start(context.Background(), params)
	defer hub.Close()

	require.NoError(t, err)
	assert.Equal(t, daemonproto.ActionStartReporting, caller.gotAction)

	wireParams, ok := caller.gotParams.(daemonproto.StartReportingParams)
	require.True(t, ok)
	assert.Equal(t, "10.0.0.5", wireParams.Host)
	assert.Equal(t, "eth0", wireParams.InterfaceID)
	assert.Equal(t, "READ_ONLY", wireParams.AccessMode)
	assert.Equal(t, []string{"CONTROL", "OTHER"}, wireParams.LnCategories)

	assert.Equal(t, 7, device.ID)
	assert.Equal(t, 9007, device.WSPort)
	assert.Equal(t, domain.DefaultMMSPort, device.MMSPort)
	assert.Equal(t, params, device.StartParams)
	assert.NotNil(t, hub)
}

func TestGateway_Start_PropagatesCallError(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrHostAlreadyRunning, Message: "already running"}
	caller := &mockCaller{err: wantErr}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	gw := NewGateway(caller, ctx, nil)

	device, hub, err := gw.Start(context.Background(), domain.StartParams{Host: "10.0.0.5"})

	assert.Equal(t, wantErr, err)
	assert.Nil(t, hub)
	assert.Equal(t, domain.Device{}, device)
}

func TestGateway_Start_PropagatesMalformedResultError(t *testing.T) {
	caller := &mockCaller{result: json.RawMessage(`not json`)}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	gw := NewGateway(caller, ctx, nil)

	_, hub, err := gw.Start(context.Background(), domain.StartParams{Host: "10.0.0.5"})

	require.Error(t, err)
	assert.Nil(t, hub)
}

func TestGateway_Stop_IssuesStopReportingWithDeviceID(t *testing.T) {
	caller := &mockCaller{result: json.RawMessage(`{"deviceId":3}`)}
	gw := NewGateway(caller, context.Background(), nil)

	err := gw.Stop(context.Background(), 3)

	require.NoError(t, err)
	assert.Equal(t, daemonproto.ActionStopReporting, caller.gotAction)
	assert.Equal(t, daemonproto.StopReportingParams{DeviceID: 3}, caller.gotParams)
}

func TestGateway_Stop_PropagatesCallError(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound, Message: "unknown device"}
	caller := &mockCaller{err: wantErr}
	gw := NewGateway(caller, context.Background(), nil)

	err := gw.Stop(context.Background(), 999)

	assert.Equal(t, wantErr, err)
}

func TestGateway_StopByAddress_IssuesStopReportingWithHostAndPort_ReturnsResolvedDeviceID(t *testing.T) {
	caller := &mockCaller{result: json.RawMessage(`{"deviceId":11}`)}
	gw := NewGateway(caller, context.Background(), nil)

	deviceID, err := gw.StopByAddress(context.Background(), "10.0.0.9", 10301)

	require.NoError(t, err)
	assert.Equal(t, daemonproto.ActionStopReporting, caller.gotAction)
	assert.Equal(t, daemonproto.StopReportingParams{Host: "10.0.0.9", MMSPort: 10301}, caller.gotParams)
	assert.Equal(t, 11, deviceID)
}

func TestGateway_StopByAddress_PropagatesCallError(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound, Message: "nothing registered here"}
	caller := &mockCaller{err: wantErr}
	gw := NewGateway(caller, context.Background(), nil)

	deviceID, err := gw.StopByAddress(context.Background(), "10.0.0.9", 102)

	assert.Equal(t, wantErr, err)
	assert.Zero(t, deviceID)
}

func TestGateway_StopByAddress_PropagatesMalformedResultError(t *testing.T) {
	caller := &mockCaller{result: json.RawMessage(`not json`)}
	gw := NewGateway(caller, context.Background(), nil)

	_, err := gw.StopByAddress(context.Background(), "10.0.0.9", 102)

	require.Error(t, err)
}
