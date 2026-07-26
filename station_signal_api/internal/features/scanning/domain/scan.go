// Package domain holds scanning's pure vocabulary: the Scan entity and the params needed to
// start one. No I/O, no imports of this feature's data/service layers.
package domain

// DefaultMMSPort is used whenever a StartParams' MMSPort is left unset (0).
const DefaultMMSPort = 102

// StartParams is this feature's own vocabulary for starting a subnet scan.
type StartParams struct {
	InterfaceID     string `json:"interfaceId"`
	MMSPort         int    `json:"mmsPort,omitempty"`
	SweepIntervalMs int    `json:"sweepIntervalMs,omitempty"`
}

// EffectiveMMSPort returns mmsPort, or DefaultMMSPort if mmsPort is unset.
func EffectiveMMSPort(mmsPort int) int {
	if mmsPort == 0 {
		return DefaultMMSPort
	}
	return mmsPort
}

// Scan is one active START_SCAN session. This is part of the feature's public contract
// (returned by service.List/service.Start), so its JSON tags are the REST response shape
// controllers/rest serializes directly — StartParams is excluded since it's only needed
// internally for crash re-arm.
type Scan struct {
	ID              int         `json:"scanId"`
	InterfaceID     string      `json:"interfaceId"`
	MMSPort         int         `json:"mmsPort"`
	SweepIntervalMs int         `json:"sweepIntervalMs"`
	StartParams     StartParams `json:"-"`
}
