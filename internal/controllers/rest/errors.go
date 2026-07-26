package rest

import (
	"encoding/json"
	"errors"
	"net/http"
	"strconv"

	"github.com/go-chi/chi/v5"

	"ied_reporter_api/internal/core/daemonproto"
)

// errorStatus maps a daemonproto.Error.Code to the HTTP status this API responds with. This
// API doesn't invent a per-feature error vocabulary — it only ever relays exactly what the
// daemon's control channel reports (plus the synthetic DAEMON_UNREACHABLE case), so mapping
// straight off daemonproto's codes here is correct rather than a layering shortcut.
func errorStatus(code string) int {
	switch code {
	case daemonproto.ErrInvalidArgument:
		return http.StatusBadRequest
	case daemonproto.ErrHostAlreadyRunning, daemonproto.ErrStartInProgress:
		return http.StatusConflict
	case daemonproto.ErrDeviceNotFound, daemonproto.ErrScanNotFound:
		return http.StatusNotFound
	case daemonproto.ErrOrchestrationFailed:
		return http.StatusBadGateway
	case daemonproto.ErrAuthRequired:
		return http.StatusUnauthorized
	case daemonproto.ErrOutOfMemory, daemonproto.ErrPortExhausted,
		daemonproto.ErrDispatcherStartFailed, daemonproto.ErrThreadCreateFailed,
		daemonproto.ErrDiscoveryCreateFailed, daemonproto.ErrDaemonUnreachable:
		return http.StatusServiceUnavailable
	default:
		return http.StatusInternalServerError
	}
}

// writeError renders err as a JSON error body with the appropriate status code. If err
// isn't a *daemonproto.Error (i.e. it's a bug on this side, not a daemon-reported
// condition), it's logged and reported as a generic 500 without leaking internals.
func (a *API) writeError(w http.ResponseWriter, err error) {
	var derr *daemonproto.Error
	if !errors.As(err, &derr) {
		a.logger.Error("unexpected internal error handling request", "error", err)
		writeJSON(w, http.StatusInternalServerError, map[string]any{
			"error": map[string]any{"code": "INTERNAL", "message": "internal error"},
		})
		return
	}
	writeJSON(w, errorStatus(derr.Code), map[string]any{"error": derr})
}

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
}

func invalidArgument(msg string) *daemonproto.Error {
	return &daemonproto.Error{Code: daemonproto.ErrInvalidArgument, Message: msg}
}

func parseIDParam(r *http.Request) (int, error) {
	raw := chi.URLParam(r, "id")
	id, err := strconv.Atoi(raw)
	if err != nil {
		return 0, invalidArgument("invalid id path parameter")
	}
	return id, nil
}
