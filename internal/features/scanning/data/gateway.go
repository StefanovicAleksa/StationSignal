package data

import (
	"context"
	"encoding/json"
	"fmt"

	"ied_reporter_api/internal/core/daemonclient"
	"ied_reporter_api/internal/core/daemonproto"
	"ied_reporter_api/internal/features/scanning/domain"
)

// Gateway is this feature's one point of contact with the daemon's control channel. It's an
// interface (rather than free functions) purely so service-layer unit tests can substitute a
// mock instead of a real daemonclient.Caller.
type Gateway interface {
	// Start issues START_SCAN for params.
	Start(ctx context.Context, params domain.StartParams) (domain.Scan, error)
	// Stop issues STOP_SCAN for scanID.
	Stop(ctx context.Context, scanID int) error
}

type daemonGateway struct {
	client daemonclient.Caller
}

// NewGateway builds the real Gateway, backed by client.
func NewGateway(client daemonclient.Caller) Gateway {
	return &daemonGateway{client: client}
}

func (g *daemonGateway) Start(ctx context.Context, params domain.StartParams) (domain.Scan, error) {
	wireParams := daemonproto.StartScanParams{
		InterfaceID:     params.InterfaceID,
		MMSPort:         params.MMSPort,
		SweepIntervalMs: params.SweepIntervalMs,
	}

	raw, err := g.client.Call(ctx, daemonproto.ActionStartScan, wireParams)
	if err != nil {
		return domain.Scan{}, err
	}

	var result daemonproto.StartScanResult
	if err := json.Unmarshal(raw, &result); err != nil {
		return domain.Scan{}, fmt.Errorf("decode START_SCAN result: %w", err)
	}

	return domain.Scan{
		ID:              result.ScanID,
		InterfaceID:     params.InterfaceID,
		MMSPort:         domain.EffectiveMMSPort(params.MMSPort),
		SweepIntervalMs: params.SweepIntervalMs,
		StartParams:     params,
	}, nil
}

func (g *daemonGateway) Stop(ctx context.Context, scanID int) error {
	_, err := g.client.Call(ctx, daemonproto.ActionStopScan, daemonproto.StopScanParams{ScanID: scanID})
	return err
}
