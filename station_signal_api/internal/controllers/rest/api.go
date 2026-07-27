// Package rest exposes the REST control surface the frontend uses to start/stop reporting
// and scanning, plus a health endpoint. It calls into each feature's service/ package only —
// never into a feature's domain/data layers directly (domain types are the exception, since
// they're part of each feature's public contract). Streaming data lives in controllers/ws.
package rest

import (
	"context"
	"io"
	"log/slog"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/go-chi/cors"

	"station_signal_api/internal/core/session"
	reportingdomain "station_signal_api/internal/features/reporting/domain"
	scanningdomain "station_signal_api/internal/features/scanning/domain"
)

// reportingService, scanningService, daemonStatus, and daemonSupervisor are the seams unit
// tests mock instead of the concrete feature services / core types. The real
// *reportingsvc.Service, *scanningsvc.Service, *daemonclient.Client, and
// *supervisionsvc.Supervisor all satisfy these implicitly — main.go needs no changes.
type reportingService interface {
	Start(ctx context.Context, sessionID string, params reportingdomain.StartParams) (reportingdomain.Device, error)
	Stop(ctx context.Context, sessionID string, deviceID int) error
	ListForSession(sessionID string) []reportingdomain.Device
}

type scanningService interface {
	Start(ctx context.Context, sessionID string, params scanningdomain.StartParams) (scanningdomain.Scan, error)
	Stop(ctx context.Context, sessionID string, scanID int) error
	ListForSession(sessionID string) []scanningdomain.Scan
}

type daemonStatus interface {
	Connected() bool
}

type daemonSupervisor interface {
	Running() bool
}

// structureFileStore is the seam unit tests mock instead of the concrete
// *structurefiles.Store — it saves an uploaded SCL/ICD/CID file to disk and returns the path
// the daemon can later read it back from.
type structureFileStore interface {
	Save(originalName string, r io.Reader) (string, error)
}

// API holds the dependencies every REST handler needs.
type API struct {
	reporting      reportingService
	scanning       scanningService
	supervisor     daemonSupervisor
	daemon         daemonStatus
	structureFiles structureFileStore
	logger         *slog.Logger
}

// New builds an API. Pass the result to Router to get a *chi.Mux.
func New(reporting reportingService, scanning scanningService, supervisor daemonSupervisor, daemon daemonStatus, structureFiles structureFileStore, logger *slog.Logger) *API {
	if logger == nil {
		logger = slog.Default()
	}
	return &API{reporting: reporting, scanning: scanning, supervisor: supervisor, daemon: daemon, structureFiles: structureFiles, logger: logger}
}

// Router builds the chi router for every REST endpoint this API exposes. It returns
// *chi.Mux (not just http.Handler) so callers — namely controllers/ws — can mount further
// routes onto the same mux.
func Router(api *API) *chi.Mux {
	r := chi.NewRouter()
	r.Use(middleware.Logger)
	r.Use(middleware.Recoverer)
	// This is a single-box, single-substation deployment: the frontend dev server (Vite's
	// default port) is the only expected caller, so the allowlist is fixed rather than
	// configurable. See station_signal_api/CLAUDE.md for the single-box deployment model.
	r.Use(cors.Handler(cors.Options{
		AllowedOrigins: []string{"http://localhost:5173"},
		AllowedMethods: []string{"GET", "POST", "DELETE", "OPTIONS"},
		AllowedHeaders: []string{"Content-Type"},
	}))
	// Every browser gets its own session cookie so scans/device-reporting one browser starts
	// aren't visible to or stoppable by another (see internal/core/session). This also covers
	// the websocket endpoints controllers/ws mounts onto this same *chi.Mux (see main.go) —
	// chi applies Use() middleware to routes registered on the mux afterward too.
	r.Use(session.Middleware)

	r.Get("/health", api.handleHealth)

	r.Get("/devices", api.handleListDevices)
	r.Post("/devices", api.handleStartReporting)
	r.Delete("/devices/{id}", api.handleStopReporting)

	r.Get("/scans", api.handleListScans)
	r.Post("/scans", api.handleStartScan)
	r.Delete("/scans/{id}", api.handleStopScan)

	r.Post("/structure-files", api.handleUploadStructureFile)

	return r
}
