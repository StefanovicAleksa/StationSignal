package domain

// Error codes this feature can produce. Unlike daemonproto's error codes (which mirror the
// daemon's own control-channel contract), these are entirely local to this feature — network
// reconfiguration never goes through the daemon.
const (
	ErrInvalidArgument      = "INVALID_ARGUMENT"
	ErrSessionsActive       = "SESSIONS_ACTIVE"
	ErrChangeAlreadyPending = "CHANGE_ALREADY_PENDING"
	ErrNoPendingChange      = "NO_PENDING_CHANGE"
	ErrApplyFailed          = "APPLY_FAILED"
)

// Error is this feature's own error shape — see internal/controllers/rest/network.go for how
// it's mapped to an HTTP status and JSON error body.
type Error struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (e *Error) Error() string {
	if e == nil {
		return ""
	}
	return e.Code + ": " + e.Message
}
