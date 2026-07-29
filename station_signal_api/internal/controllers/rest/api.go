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
	networkdomain "station_signal_api/internal/features/network/domain"
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

// networkService is the seam for the box's static-IP reconfiguration feature — see
// internal/features/network/service for the real implementation and
// station_signal_api/CLAUDE.md-adjacent design notes on the provisional-apply/confirm/
// auto-revert flow.
type networkService interface {
	GetStatus(ctx context.Context) (networkdomain.Status, error)
	Apply(ctx context.Context, cfg networkdomain.Config) (networkdomain.PendingChange, error)
	Confirm(ctx context.Context) error
	Revert(ctx context.Context) error
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
	network        networkService
	logger         *slog.Logger
}

// New builds an API. Pass the result to Router to get a *chi.Mux.
func New(reporting reportingService, scanning scanningService, supervisor daemonSupervisor, daemon daemonStatus, structureFiles structureFileStore, network networkService, logger *slog.Logger) *API {
	if logger == nil {
		logger = slog.Default()
	}
	return &API{reporting: reporting, scanning: scanning, supervisor: supervisor, daemon: daemon, structureFiles: structureFiles, network: network, logger: logger}
}

// Router builds the chi router for every REST endpoint this API exposes. It returns
// *chi.Mux (not just http.Handler) so callers — namely controllers/ws — can mount further
// routes onto the same mux.
func Router(api *API) *chi.Mux {
	r := chi.NewRouter()
	r.Use(middleware.Logger)
	r.Use(middleware.Recoverer)
	// No auth, no TLS, local-network trust model (see station_signal_api/CLAUDE.md) — and the
	// Settings page's network-reconfiguration flow needs cross-origin GET /health and POST
	// /settings/network/confirm to work against whatever new address it just configured, from
	// whatever origin the technician's browser happens to be on (production nginx origin, the
	// fixed recovery address, or the Vite dev server) — a fixed single-origin allowlist can never
	// cover that, so any origin is allowed rather than guessing at one. No credentials are ever
	// sent cross-origin (session cookies are scoped per-origin and only matter for same-origin
	// calls anyway), so this doesn't widen anything sensitive.
	r.Use(cors.Handler(cors.Options{
		AllowedOrigins: []string{"*"},
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

	r.Get("/settings/network", api.handleGetNetworkStatus)
	r.Post("/settings/network", api.handleApplyNetworkConfig)
	r.Post("/settings/network/confirm", api.handleConfirmNetworkConfig)
	r.Post("/settings/network/revert", api.handleRevertNetworkConfig)

	return r
}
