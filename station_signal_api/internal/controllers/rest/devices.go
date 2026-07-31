package rest

import (
	"encoding/json"
	"errors"
	"net/http"
	"strconv"
	"strings"

	"station_signal_api/internal/core/daemonproto"
	"station_signal_api/internal/core/session"
	reportingdomain "station_signal_api/internal/features/reporting/domain"
)

// classifyStartReportingError recognizes the daemon's existing SCL-bootstrap
// access-denied signature (ORCHESTRATION_FAILED / stage="SCL bootstrap" /
// detail containing "access denied") and synthesizes a distinct
// daemonproto.ErrAuthRequired in its place, so the frontend doesn't have to
// pattern-match a free-text detail string itself to know a password is
// needed. Every other error passes through unchanged. This only classifies —
// it never changes what the daemon actually reports (see this package's own
// errors.go doc comment on why relaying, not redesigning, is the rule here).
func classifyStartReportingError(err error) error {
	var derr *daemonproto.Error
	if !errors.As(err, &derr) || derr.Code != daemonproto.ErrOrchestrationFailed {
		return err
	}
	if derr.Stage == nil || *derr.Stage != "SCL bootstrap" {
		return err
	}
	if derr.Detail == nil || !strings.Contains(*derr.Detail, "access denied") {
		return err
	}

	return &daemonproto.Error{
		Code:    daemonproto.ErrAuthRequired,
		Message: "the device requires ACSE authentication (acseAuthPassword) to connect",
		Stage:   derr.Stage,
		Detail:  derr.Detail,
	}
}

func (a *API) handleStartReporting(w http.ResponseWriter, r *http.Request) {
	var params reportingdomain.StartParams
	if err := json.NewDecoder(r.Body).Decode(&params); err != nil {
		a.writeError(w, invalidArgument("malformed JSON body: "+err.Error()))
		return
	}

	device, err := a.reporting.Start(r.Context(), session.FromContext(r.Context()), params)
	if err != nil {
		a.writeError(w, classifyStartReportingError(err))
		return
	}

	writeJSON(w, http.StatusCreated, map[string]any{
		"deviceId":       device.ID,
		"wsPort":         device.WSPort,
		"mmsAvailable":   device.MMSAvailable,
		"gooseAvailable": device.GooseAvailable,
	})
}

func (a *API) handleStopReporting(w http.ResponseWriter, r *http.Request) {
	id, err := parseIDParam(r)
	if err != nil {
		a.writeError(w, err)
		return
	}

	if err := a.reporting.Stop(r.Context(), session.FromContext(r.Context()), id); err != nil {
		a.writeError(w, err)
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{"deviceId": id})
}

func (a *API) handleListDevices(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, a.reporting.ListForSession(session.FromContext(r.Context())))
}

// handleStopReportingByAddress is the recovery path for a device this API never obtained (or has
// lost track of) a deviceId for — see Service.StopByAddress's own doc comment. Collection-level
// (query params, not a path {id}) since there is no id to put in the path by construction.
func (a *API) handleStopReportingByAddress(w http.ResponseWriter, r *http.Request) {
	host := r.URL.Query().Get("host")
	if host == "" {
		a.writeError(w, invalidArgument("host query parameter is required"))
		return
	}

	mmsPort := reportingdomain.DefaultMMSPort
	if raw := r.URL.Query().Get("mmsPort"); raw != "" {
		parsed, err := strconv.Atoi(raw)
		if err != nil {
			a.writeError(w, invalidArgument("invalid mmsPort query parameter"))
			return
		}
		mmsPort = parsed
	}

	if err := a.reporting.StopByAddress(r.Context(), host, mmsPort); err != nil {
		a.writeError(w, err)
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{"host": host, "mmsPort": mmsPort})
}
