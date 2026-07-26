package rest

import (
	"encoding/json"
	"errors"
	"net/http"
	"strings"

	"station_signal_api/internal/core/daemonproto"
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

	device, err := a.reporting.Start(r.Context(), params)
	if err != nil {
		a.writeError(w, classifyStartReportingError(err))
		return
	}

	writeJSON(w, http.StatusCreated, map[string]any{"deviceId": device.ID, "wsPort": device.WSPort})
}

func (a *API) handleStopReporting(w http.ResponseWriter, r *http.Request) {
	id, err := parseIDParam(r)
	if err != nil {
		a.writeError(w, err)
		return
	}

	if err := a.reporting.Stop(r.Context(), id); err != nil {
		a.writeError(w, err)
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{"deviceId": id})
}

func (a *API) handleListDevices(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, a.reporting.List())
}
