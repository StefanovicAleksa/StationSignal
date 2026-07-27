package rest

import (
	"encoding/json"
	"net/http"

	"station_signal_api/internal/core/session"
	scanningdomain "station_signal_api/internal/features/scanning/domain"
)

func (a *API) handleStartScan(w http.ResponseWriter, r *http.Request) {
	var params scanningdomain.StartParams
	if err := json.NewDecoder(r.Body).Decode(&params); err != nil {
		a.writeError(w, invalidArgument("malformed JSON body: "+err.Error()))
		return
	}

	scan, err := a.scanning.Start(r.Context(), session.FromContext(r.Context()), params)
	if err != nil {
		a.writeError(w, err)
		return
	}

	writeJSON(w, http.StatusCreated, map[string]any{"scanId": scan.ID})
}

func (a *API) handleStopScan(w http.ResponseWriter, r *http.Request) {
	id, err := parseIDParam(r)
	if err != nil {
		a.writeError(w, err)
		return
	}

	if err := a.scanning.Stop(r.Context(), session.FromContext(r.Context()), id); err != nil {
		a.writeError(w, err)
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{"scanId": id})
}

func (a *API) handleListScans(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, a.scanning.ListForSession(session.FromContext(r.Context())))
}
