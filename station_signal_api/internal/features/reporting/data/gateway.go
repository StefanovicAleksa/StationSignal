package data

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"

	"station_signal_api/internal/core/daemonclient"
	"station_signal_api/internal/core/daemonproto"
	"station_signal_api/internal/core/streamrelay"
	"station_signal_api/internal/features/reporting/domain"
)

// Gateway is this feature's one point of contact with the daemon's control channel and
// per-device stream port. It's an interface (rather than free functions) purely so
// service-layer unit tests can substitute a mock instead of a real daemonclient.Caller.
type Gateway interface {
	// Start issues START_REPORTING for params, then dials the returned device's stream hub.
	Start(ctx context.Context, params domain.StartParams) (domain.Device, *streamrelay.Hub, error)
	// Stop issues STOP_REPORTING for deviceID.
	Stop(ctx context.Context, deviceID int) error
}

type daemonGateway struct {
	client daemonclient.Caller
	hubCtx context.Context
	logger *slog.Logger
}

// NewGateway builds the real Gateway, backed by client. hubCtx is the parent context for
// every device stream hub's upstream connection goroutine.
func NewGateway(client daemonclient.Caller, hubCtx context.Context, logger *slog.Logger) Gateway {
	return &daemonGateway{client: client, hubCtx: hubCtx, logger: logger}
}

func (g *daemonGateway) Start(ctx context.Context, params domain.StartParams) (domain.Device, *streamrelay.Hub, error) {
	wireParams := daemonproto.StartReportingParams{
		Host:             params.Host,
		MMSPort:          params.MMSPort,
		IEDName:          params.IEDName,
		InterfaceID:      params.InterfaceID,
		SCLFilePath:      params.SCLFilePath,
		ACSEAuthPassword: params.ACSEAuthPassword,
		AccessMode:       string(params.AccessMode),
	}

	raw, err := g.client.Call(ctx, daemonproto.ActionStartReporting, wireParams)
	if err != nil {
		return domain.Device{}, nil, err
	}

	var result daemonproto.StartReportingResult
	if err := json.Unmarshal(raw, &result); err != nil {
		return domain.Device{}, nil, fmt.Errorf("decode START_REPORTING result: %w", err)
	}

	device := domain.Device{
		ID:          result.DeviceID,
		Host:        params.Host,
		MMSPort:     domain.EffectiveMMSPort(params.MMSPort),
		InterfaceID: params.InterfaceID,
		WSPort:      result.WSPort,
		StartParams: params,
	}
	hub := streamrelay.NewHub(g.hubCtx, fmt.Sprintf("ws://127.0.0.1:%d", result.WSPort), g.logger)
	return device, hub, nil
}

func (g *daemonGateway) Stop(ctx context.Context, deviceID int) error {
	_, err := g.client.Call(ctx, daemonproto.ActionStopReporting, daemonproto.StopReportingParams{DeviceID: deviceID})
	return err
}
