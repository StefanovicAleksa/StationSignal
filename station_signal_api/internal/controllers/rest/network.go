package rest

import (
	"encoding/json"
	"net/http"
	"time"

	networkdomain "station_signal_api/internal/features/network/domain"
)

// pendingChangeResponse is the wire shape for a pending change. expiresAt is the box's own
// clock's view of the deadline, kept for display only — remainingSeconds is what the frontend
// actually gates its reconnect-confirmation loop on (stores/settings.ts). A relative duration
// needs no agreement between the box's clock and the browser's; comparing an absolute timestamp
// across the two breaks silently whenever the box's clock is wrong, e.g. a bare direct-Ethernet
// link with no upstream internet/NTP — the box then reports "not confirmed reachable" even though
// the address change and its auto-revert both worked correctly.
type pendingChangeResponse struct {
	New              networkdomain.Config `json:"new"`
	ExpiresAt        time.Time            `json:"expiresAt"`
	RemainingSeconds int                  `json:"remainingSeconds"`
}

func toPendingChangeResponse(p networkdomain.PendingChange) pendingChangeResponse {
	remaining := int(time.Until(p.ExpiresAt).Seconds())
	if remaining < 0 {
		remaining = 0
	}
	return pendingChangeResponse{New: p.New, ExpiresAt: p.ExpiresAt, RemainingSeconds: remaining}
}

type statusResponse struct {
	Interface       string                 `json:"interface"`
	Current         networkdomain.Config   `json:"current"`
	RecoveryAddress string                 `json:"recoveryAddress"`
	Pending         *pendingChangeResponse `json:"pending,omitempty"`
}

func toStatusResponse(s networkdomain.Status) statusResponse {
	resp := statusResponse{
		Interface:       s.Interface,
		Current:         s.Current,
		RecoveryAddress: s.RecoveryAddress,
	}
	if s.Pending != nil {
		pc := toPendingChangeResponse(*s.Pending)
		resp.Pending = &pc
	}
	return resp
}

func (a *API) handleGetNetworkStatus(w http.ResponseWriter, r *http.Request) {
	status, err := a.network.GetStatus(r.Context())
	if err != nil {
		a.writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, toStatusResponse(status))
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

	writeJSON(w, http.StatusAccepted, toPendingChangeResponse(pending))
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

// handleRevertNetworkConfig undoes a provisionally-applied change without waiting out the
// OS-level auto-revert, and clears the pending state that would otherwise refuse every later
// apply. It's the Settings page's recovery action for a change that never came up — and for a
// marker an earlier failed revert left behind, which is only reachable this way short of shell
// access to the box.
func (a *API) handleRevertNetworkConfig(w http.ResponseWriter, r *http.Request) {
	if err := a.network.Revert(r.Context()); err != nil {
		a.writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"reverted": true})
}
