// Package domain holds reporting's pure vocabulary: the Device entity, the params needed to
// start reporting on one, and small pure helpers. No I/O, no imports of this feature's
// data/service layers, no dependency on the daemon's wire format (data/ maps between the two).
package domain

// AccessMode is the level of MMS access requested when starting reporting on a device.
type AccessMode string

const (
	AccessModeReportOnly AccessMode = "REPORT_ONLY"
	AccessModeReadOnly   AccessMode = "READ_ONLY"
	AccessModeReadWrite  AccessMode = "READ_AND_WRITE"
)

// DefaultMMSPort is used whenever a StartParams' MMSPort is left unset (0).
const DefaultMMSPort = 102

// StartParams is this feature's own vocabulary for starting reporting on one IED.
type StartParams struct {
	Host             string     `json:"host"`
	MMSPort          int        `json:"mmsPort,omitempty"`
	IEDName          string     `json:"iedName,omitempty"`
	InterfaceID      string     `json:"interfaceId"`
	SCLFilePath      string     `json:"sclFilePath,omitempty"`
	ACSEAuthPassword string     `json:"acseAuthPassword,omitempty"`
	AccessMode       AccessMode `json:"accessMode,omitempty"`
}

// EffectiveMMSPort returns mmsPort, or DefaultMMSPort if mmsPort is unset.
func EffectiveMMSPort(mmsPort int) int {
	if mmsPort == 0 {
		return DefaultMMSPort
	}
	return mmsPort
}

// Device is one active START_REPORTING session. This is part of the feature's public
// contract (returned by service.List/service.Start), so its JSON tags are the REST response
// shape controllers/rest serializes directly — StartParams is excluded since it's only
// needed internally for crash re-arm.
type Device struct {
	ID          int    `json:"deviceId"`
	Host        string `json:"host"`
	MMSPort     int    `json:"mmsPort"`
	InterfaceID string `json:"interfaceId"`
	WSPort      int    `json:"wsPort"`
	// MMSAvailable/GooseAvailable report which of MMS reporting / GOOSE subscription this
	// device's SCL actually declared targets for — a device declaring only one of the two
	// still starts successfully (see the daemon's own ORCHESTRATION_ERR_NO_CAPABILITIES),
	// so the frontend needs this to know which stream(s) to expect data on.
	MMSAvailable   bool        `json:"mmsAvailable"`
	GooseAvailable bool        `json:"gooseAvailable"`
	StartParams    StartParams `json:"-"`
	// SessionID is the browser session that started reporting on this device (see
	// internal/core/session) — not daemon-facing, only used to scope this API's own
	// List/Stream/Stop to their owner.
	SessionID string `json:"-"`
}
