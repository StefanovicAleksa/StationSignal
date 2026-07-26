package ws

import (
	"net/http"
	"strconv"

	"github.com/go-chi/chi/v5"
)

func (a *API) handleDeviceStream(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.Atoi(chi.URLParam(r, "id"))
	if err != nil {
		http.Error(w, "invalid id path parameter", http.StatusBadRequest)
		return
	}

	ch, cancel, ok := a.reporting.StreamFor(id)
	if !ok {
		http.Error(w, "device not found or not active", http.StatusNotFound)
		return
	}
	defer cancel()

	conn, err := a.upgrader.Upgrade(w, r, nil)
	if err != nil {
		a.logger.Warn("device stream ws upgrade failed", "deviceId", id, "error", err)
		return
	}
	defer conn.Close()

	closed := watchForClose(conn)
	pump(conn, ch, closed)
}
