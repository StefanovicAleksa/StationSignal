package data

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/daemonproto"
	"station_signal_api/internal/features/scanning/domain"
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
	caller := &mockCaller{result: json.RawMessage(`{"scanId":7}`)}
	gw := NewGateway(caller)

	params := domain.StartParams{InterfaceID: "eth0", SweepIntervalMs: 5000}
	scan, err := gw.Start(context.Background(), params)

	require.NoError(t, err)
	assert.Equal(t, daemonproto.ActionStartScan, caller.gotAction)

	wireParams, ok := caller.gotParams.(daemonproto.StartScanParams)
	require.True(t, ok)
	assert.Equal(t, "eth0", wireParams.InterfaceID)
	assert.Equal(t, 5000, wireParams.SweepIntervalMs)

	assert.Equal(t, 7, scan.ID)
	assert.Equal(t, domain.DefaultMMSPort, scan.MMSPort)
	assert.Equal(t, params, scan.StartParams)
}

func TestGateway_Start_PropagatesCallError(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrDispatcherStartFailed}
	caller := &mockCaller{err: wantErr}
	gw := NewGateway(caller)

	scan, err := gw.Start(context.Background(), domain.StartParams{InterfaceID: "eth0"})

	assert.Equal(t, wantErr, err)
	assert.Equal(t, domain.Scan{}, scan)
}

func TestGateway_Start_PropagatesMalformedResultError(t *testing.T) {
	caller := &mockCaller{result: json.RawMessage(`not json`)}
	gw := NewGateway(caller)

	_, err := gw.Start(context.Background(), domain.StartParams{InterfaceID: "eth0"})

	assert.Error(t, err)
}

func TestGateway_Stop_IssuesStopScanWithScanID(t *testing.T) {
	caller := &mockCaller{result: json.RawMessage(`{"scanId":3}`)}
	gw := NewGateway(caller)

	err := gw.Stop(context.Background(), 3)

	require.NoError(t, err)
	assert.Equal(t, daemonproto.ActionStopScan, caller.gotAction)
	assert.Equal(t, daemonproto.StopScanParams{ScanID: 3}, caller.gotParams)
}

func TestGateway_Stop_PropagatesCallError(t *testing.T) {
	wantErr := &daemonproto.Error{Code: daemonproto.ErrScanNotFound}
	caller := &mockCaller{err: wantErr}
	gw := NewGateway(caller)

	err := gw.Stop(context.Background(), 999)

	assert.Equal(t, wantErr, err)
}
