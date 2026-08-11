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

	// Uploaded structure files are swept once they go unreferenced for a while, and a device
	// mid-start is not yet in the registry the sweep consults. Touching the file first restarts
	// its clock, so a technician who browsed a file, got distracted, and only then hit Connect
	// can't have it deleted between this request and the daemon reading it. A no-op for an
	// sclFilePath that isn't one of our uploads (any path on the daemon's disk is legal).
	if params.SCLFilePath != "" {
		a.structureFiles.Touch(params.SCLFilePath)
	}

	device, err := a.reporting.Start(r.Context(), session.FromContext(r.Context()), params)
	if err != nil {
		a.writeError(w, classifyStartReportingError(err))
		return
	}

	// Serialize the domain entity itself rather than hand-picking fields into a map, same as
	// handleListDevices — domain.Device's JSON tags *are* this response shape. A hand-built map
	// silently dropped lnCategories, and since an absent lnCategories means "unfiltered" by
	// contract, every client that asked for a category filter was told the running device had
	// none and concluded it had been attached to someone else's device. The struct's `omitempty`
	// also omits the key for a genuinely unfiltered device, where a map would emit a null.
	writeJSON(w, http.StatusCreated, device)
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
