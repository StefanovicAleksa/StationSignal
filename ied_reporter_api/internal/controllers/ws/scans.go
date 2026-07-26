package ws

import (
	"encoding/json"
	"net/http"
	"time"

	"github.com/gorilla/websocket"

	"ied_reporter_api/internal/core/daemonproto"
)

// handleScanStream relays the shared scan-result stream. If no scan is currently active it
// accepts the upgrade and idles rather than rejecting — consistent with the daemon's own
// "silence means nothing changed" semantics — and starts relaying as soon as a scan starts.
func (a *API) handleScanStream(w http.ResponseWriter, r *http.Request) {
	conn, err := a.upgrader.Upgrade(w, r, nil)
	if err != nil {
		a.logger.Warn("scan stream ws upgrade failed", "error", err)
		return
	}
	defer conn.Close()

	closed := watchForClose(conn)

	ticker := time.NewTicker(scanHubPollPeriod)
	defer ticker.Stop()
	for {
		if ch, cancel, ok := a.scanning.StreamScans(); ok {
			pumpScans(conn, ch, closed, a.reporting)
			cancel()
			// pumpScans returns when ch closes (hub torn down, scan count hit zero) or the
			// client disconnected (closed fired). Only the latter should end this connection —
			// drop back to idle-polling so an already-open client is ready for the next scan
			// without a fresh handshake.
		}
		select {
		case <-closed:
			return
		case <-ticker.C:
		}
	}
}

// pumpScans forwards every scan-result message on ch to conn, omitting devices that already
// have an active reporting session — those are still discovered by the daemon and will reappear
// in a later scan once disconnected, they just aren't surfaced to the frontend while connected.
// Messages that don't parse as a scan result are forwarded unfiltered, consistent with the rest
// of this relay never validating the daemon's wire format.
func pumpScans(conn *websocket.Conn, ch <-chan []byte, closed <-chan struct{}, reporting reportingStreamer) {
	for {
		select {
		case msg, ok := <-ch:
			if !ok {
				return
			}
			var sr daemonproto.ScanResultMessage
			if err := json.Unmarshal(msg, &sr); err == nil && reporting.IsConnected(sr.Host, sr.MMSPort) {
				continue
			}
			_ = conn.SetWriteDeadline(time.Now().Add(writeTimeout))
			if err := conn.WriteMessage(websocket.TextMessage, msg); err != nil {
				return
			}
		case <-closed:
			return
		}
	}
}
