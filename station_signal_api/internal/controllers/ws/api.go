// Package ws exposes the frontend-facing websocket endpoints that relay live data out of
// each feature's service/ package (ReportingService.StreamFor, ScanningService.StreamScans).
// The frontend never dials the daemon's own ports directly, and this package never touches
// core/streamrelay itself — that stays fully behind each feature's service boundary.
package ws

import (
	"log/slog"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/gorilla/websocket"
)

const (
	writeTimeout      = 5 * time.Second
	scanHubPollPeriod = 20 * time.Millisecond
)

// reportingStreamer and scanningStreamer are the seams unit tests mock instead of the
// concrete feature services. The real *reportingsvc.Service/*scanningsvc.Service satisfy
// these implicitly — main.go needs no changes.
type reportingStreamer interface {
	StreamFor(deviceID int) (<-chan []byte, func(), bool)
	IsConnected(sessionID, host string, mmsPort int) bool
	OwnsDevice(sessionID string, deviceID int) bool
}

type scanningStreamer interface {
	StreamScans() (<-chan []byte, func(), bool)
	OwnsScan(sessionID string, scanID int) bool
}

// API holds the dependencies every websocket handler needs.
type API struct {
	reporting reportingStreamer
	scanning  scanningStreamer
	logger    *slog.Logger
	upgrader  websocket.Upgrader
}

// New builds an API. Pass it to RegisterRoutes to wire its handlers onto a router.
func New(reporting reportingStreamer, scanning scanningStreamer, logger *slog.Logger) *API {
	if logger == nil {
		logger = slog.Default()
	}
	return &API{
		reporting: reporting,
		scanning:  scanning,
		logger:    logger,
		upgrader: websocket.Upgrader{
			// This API is designed to sit on a trusted local network (single box, single
			// substation, no multi-tenancy) — same trust model as the daemon it wraps, per
			// station_signal/CLAUDE.md. Origin checking isn't a meaningful boundary here.
			CheckOrigin: func(r *http.Request) bool { return true },
		},
	}
}

// RegisterRoutes mounts every websocket endpoint onto r.
func RegisterRoutes(r chi.Router, api *API) {
	r.Get("/ws/devices/{id}", api.handleDeviceStream)
	r.Get("/ws/scans", api.handleScanStream)
}

// pendingSetCookie carries any Set-Cookie header already added to w (e.g. by session.Middleware
// minting a fresh session cookie) into a websocket upgrade response. gorilla/websocket's
// Upgrade doesn't read w.Header() itself — it only writes whatever's passed as its own
// responseHeader argument — so a cookie minted on a request whose first contact happens to be a
// raw WS dial (no prior REST call) would otherwise never reach the browser.
func pendingSetCookie(w http.ResponseWriter) http.Header {
	cookies := w.Header().Values("Set-Cookie")
	if len(cookies) == 0 {
		return nil
	}
	h := http.Header{}
	h["Set-Cookie"] = cookies
	return h
}

// pump forwards every message on ch to conn until ch closes (the underlying device/scan went
// away) or the client disconnects.
func pump(conn *websocket.Conn, ch <-chan []byte, closed <-chan struct{}) {
	for {
		select {
		case msg, ok := <-ch:
			if !ok {
				return
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

// watchForClose reads (and discards) inbound frames so gorilla's control-frame handling
// (ping/pong/close) keeps working on this push-only connection, and signals closed once the
// client disconnects or sends a close frame.
func watchForClose(conn *websocket.Conn) <-chan struct{} {
	closed := make(chan struct{})
	go func() {
		defer close(closed)
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				return
			}
		}
	}()
	return closed
}
