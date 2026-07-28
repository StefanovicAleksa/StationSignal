package rest

import (
	"encoding/json"
	"net/http"

	networkdomain "station_signal_api/internal/features/network/domain"
)

func (a *API) handleGetNetworkStatus(w http.ResponseWriter, r *http.Request) {
	status, err := a.network.GetStatus(r.Context())
	if err != nil {
		a.writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, status)
}

// handleApplyNetworkConfig provisionally applies a new static IP for the box's LAN interface.
// POST-only (like every other mutating endpoint here) so this can't be triggered by a plain
// link/GET or an unrelated cross-origin page. The response is 202 Accepted, not 200 — the
// change isn't permanent yet, see handleConfirmNetworkConfig.
func (a *API) handleApplyNetworkConfig(w http.ResponseWriter, r *http.Request) {
	var cfg networkdomain.Config
	if err := json.NewDecoder(r.Body).Decode(&cfg); err != nil {
		a.writeError(w, networkInvalidArgument("malformed JSON body: "+err.Error()))
		return
	}

	pending, err := a.network.Apply(r.Context(), cfg)
	if err != nil {
		a.writeError(w, err)
		return
	}

	writeJSON(w, http.StatusAccepted, pending)
}

// handleConfirmNetworkConfig cancels the pending auto-revert and makes the last-applied change
// permanent. The frontend calls this against the NEW address once it's confirmed reachable —
// see src/stores/settings.ts.
func (a *API) handleConfirmNetworkConfig(w http.ResponseWriter, r *http.Request) {
	if err := a.network.Confirm(r.Context()); err != nil {
		a.writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"confirmed": true})
}
