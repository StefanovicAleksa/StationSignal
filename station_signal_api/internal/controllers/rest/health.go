package rest

import "net/http"

func (a *API) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"daemonRunning":           a.supervisor.Running(),
		"controlChannelConnected": a.daemon.Connected(),
	})
}
