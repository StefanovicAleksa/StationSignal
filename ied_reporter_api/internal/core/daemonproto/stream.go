package daemonproto

// Stream message "type" discriminator values, shared by the per-device report stream and
// the scan-result stream.
const (
	StreamTypeMMSReport  = "MMS_REPORT"
	StreamTypeGOOSE      = "GOOSE"
	StreamTypeScanResult = "SCAN_RESULT"
)

// Quality is the optional quality attribute attached to a data point.
type Quality struct {
	Validity    string `json:"validity"`
	DetailFlags int    `json:"detailFlags"`
}

// DataPoint is one changed value inside a per-device stream message.
type DataPoint struct {
	Reference       string   `json:"reference"`
	Value           any      `json:"value"`
	Quality         *Quality `json:"quality"`
	PreviousValue   any      `json:"previousValue"`
	PreviousQuality *Quality `json:"previousQuality"`
	Label           *string  `json:"label"`
	PreviousLabel   *string  `json:"previousLabel"`
}

// ReportSource is the "source" object for an MMS_REPORT message.
type ReportSource struct {
	RCBReference *string `json:"rcbReference"`
	Buffered     bool    `json:"buffered"`
}

// GOOSESource is the "source" object for a GOOSE message.
type GOOSESource struct {
	GoCBRef *string `json:"goCbRef"`
}

// DeviceStreamMessage is a single push on a per-device report stream
// (ws://127.0.0.1:<wsPort>). Exactly one of Source variants is meaningful depending on Type;
// callers should inspect Type before touching either Source field. Kept as json.RawMessage
// for Source since its shape depends on Type — decode with DecodeSource.
type DeviceStreamMessage struct {
	SchemaVersion int         `json:"schemaVersion"`
	Type          string      `json:"type"`
	HasTimestamp  bool        `json:"hasTimestamp"`
	TimestampMs   *int64      `json:"timestampMs,omitempty"`
	DataPoints    []DataPoint `json:"dataPoints"`
}

// ScanResultMessage is a single push on the shared scan-result stream (ws://127.0.0.1:8766).
type ScanResultMessage struct {
	SchemaVersion  int    `json:"schemaVersion"`
	Type           string `json:"type"`
	ScanID         int    `json:"scanId"`
	Host           string `json:"host"`
	MMSPort        int    `json:"mmsPort"`
	DiscoveredAtMs int64  `json:"discoveredAtMs"`
}
