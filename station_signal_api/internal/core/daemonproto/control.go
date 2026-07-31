// Package daemonproto holds the Go representation of station_signal_daemon's JSON wire
// contract (control channel + both push-only streams), as documented in
// station_signal_daemon/docs/AGENT_API_GUIDE.md. Nothing here should diverge from that doc —
// treat it as an external stable contract, not something to redesign.
package daemonproto

import "encoding/json"

const (
	ActionStartReporting = "START_REPORTING"
	ActionStopReporting  = "STOP_REPORTING"
	ActionStartScan      = "START_SCAN"
	ActionStopScan       = "STOP_SCAN"
)

// Daemon error.code values.
const (
	ErrInvalidArgument       = "INVALID_ARGUMENT"
	ErrOutOfMemory           = "OUT_OF_MEMORY"
	ErrPortExhausted         = "PORT_EXHAUSTED"
	ErrHostAlreadyRunning    = "HOST_ALREADY_RUNNING"
	ErrOrchestrationFailed   = "ORCHESTRATION_FAILED"
	ErrDeviceNotFound        = "DEVICE_NOT_FOUND"
	ErrStartInProgress       = "START_IN_PROGRESS"
	ErrScanNotFound          = "SCAN_NOT_FOUND"
	ErrDispatcherStartFailed = "DISPATCHER_START_FAILED"
	ErrThreadCreateFailed    = "THREAD_CREATE_FAILED"
	ErrDiscoveryCreateFailed = "DISCOVERY_CREATE_FAILED"
	ErrUnknownAction         = "UNKNOWN_ACTION"

	// ErrDaemonUnreachable is synthesized by daemonclient itself (not part of the daemon's
	// own contract) when a call can't be completed at all: no connection, or it timed out
	// waiting for a response.
	ErrDaemonUnreachable = "DAEMON_UNREACHABLE"

	// ErrAuthRequired is synthesized by rest.classifyStartReportingError (not part of the
	// daemon's own contract) by pattern-matching the daemon's own ORCHESTRATION_FAILED /
	// stage="SCL bootstrap" / detail="access denied (auth required/rejected)" signature —
	// see station_signal_daemon's orchestration_utils.c (OrchestrationUtils_stageToString /
	// OrchestrationUtils_candidateStatusToString). Surfaced when a device rejects an
	// unauthenticated (or wrongly authenticated) connection attempt during SCL bootstrap.
	ErrAuthRequired = "AUTH_REQUIRED"

	// ErrDeviceTracked is synthesized by reporting.Service.StopByAddress (not part of the
	// daemon's own contract) when this API's own store already has a record for the given
	// (host, mmsPort) — the address-based stop is a recovery path for a device this API has
	// lost track of, not a way to force-stop something already properly tracked (which may be
	// shared with another session); the caller should use the normal deviceId-based Stop
	// instead.
	ErrDeviceTracked = "DEVICE_TRACKED"
)

// Request is the inbound control-channel envelope.
type Request struct {
	RequestID string `json:"requestId"`
	Action    string `json:"action"`
	Params    any    `json:"params"`
}

// Response is the outbound control-channel envelope.
type Response struct {
	SchemaVersion int             `json:"schemaVersion"`
	RequestID     string          `json:"requestId"`
	Action        *string         `json:"action"`
	Success       bool            `json:"success"`
	Result        json.RawMessage `json:"result"`
	Error         *Error          `json:"error"`
}

// Error is the daemon's control-channel error shape.
type Error struct {
	Code    string  `json:"code"`
	Message string  `json:"message"`
	Stage   *string `json:"stage"`
	Detail  *string `json:"detail"`
}

func (e *Error) Error() string {
	if e == nil {
		return ""
	}
	return e.Code + ": " + e.Message
}

// StartReportingParams is the params object for a START_REPORTING request.
type StartReportingParams struct {
	Host             string `json:"host"`
	MMSPort          int    `json:"mmsPort,omitempty"`
	IEDName          string `json:"iedName,omitempty"`
	InterfaceID      string `json:"interfaceId"`
	SCLFilePath      string `json:"sclFilePath,omitempty"`
	ACSEAuthPassword string `json:"acseAuthPassword,omitempty"`
	AccessMode       string `json:"accessMode,omitempty"`
}

// StartReportingResult is the result object on a successful START_REPORTING response.
type StartReportingResult struct {
	DeviceID       int  `json:"deviceId"`
	WSPort         int  `json:"wsPort"`
	MMSAvailable   bool `json:"mmsAvailable"`
	GooseAvailable bool `json:"gooseAvailable"`
}

// StopReportingParams is the params object for a STOP_REPORTING request. Two mutually exclusive
// forms, matching the daemon's own parser (AGENT_API_GUIDE.md's STOP_REPORTING section):
// DeviceID (the normal case), or Host+MMSPort for a caller that never obtained or has lost track
// of a deviceId. omitempty on all three so only the fields actually set are sent — the daemon
// discriminates on whether the "deviceId" key is present at all, so a stray zero-value deviceId
// must never be serialized alongside the address form.
type StopReportingParams struct {
	DeviceID int    `json:"deviceId,omitempty"`
	Host     string `json:"host,omitempty"`
	MMSPort  int    `json:"mmsPort,omitempty"`
}

// StopReportingResult is the result object on a successful STOP_REPORTING response.
type StopReportingResult struct {
	DeviceID int `json:"deviceId"`
}

// StartScanParams is the params object for a START_SCAN request.
type StartScanParams struct {
	InterfaceID     string `json:"interfaceId"`
	MMSPort         int    `json:"mmsPort,omitempty"`
	SweepIntervalMs int    `json:"sweepIntervalMs,omitempty"`
}

// StartScanResult is the result object on a successful START_SCAN response.
type StartScanResult struct {
	ScanID int `json:"scanId"`
}

// StopScanParams is the params object for a STOP_SCAN request.
type StopScanParams struct {
	ScanID int `json:"scanId"`
}

// StopScanResult is the result object on a successful STOP_SCAN response.
type StopScanResult struct {
	ScanID int `json:"scanId"`
}
