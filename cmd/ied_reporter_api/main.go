// Command ied_reporter_api supervises a single ied_reporter_daemon process and relays
// control commands and live report/scan data between it and a frontend, over plain REST +
// websocket endpoints. See ied_reporter_api/CLAUDE.md for the service's scope.
package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"ied_reporter_api/internal/controllers/rest"
	"ied_reporter_api/internal/controllers/ws"
	"ied_reporter_api/internal/core/config"
	"ied_reporter_api/internal/core/daemonclient"
	"ied_reporter_api/internal/core/structurefiles"
	reportingsvc "ied_reporter_api/internal/features/reporting/service"
	scanningsvc "ied_reporter_api/internal/features/scanning/service"
	supervisionsvc "ied_reporter_api/internal/features/supervision/service"
)

const (
	controlAddr   = "127.0.0.1:8767"
	controlURL    = "ws://" + controlAddr
	shutdownGrace = 5 * time.Second
)

func main() {
	logger := slog.New(slog.NewTextHandler(os.Stdout, nil))

	cfg, err := config.Load(os.Args[1:])
	if err != nil {
		logger.Error("invalid configuration", "error", err)
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	sup := supervisionsvc.New(cfg.DaemonBinPath, controlAddr, logger)
	supervisorDone := make(chan struct{})
	go func() {
		defer close(supervisorDone)
		if err := sup.Run(ctx); err != nil && !errors.Is(err, context.Canceled) {
			logger.Error("daemon supervisor exited", "error", err)
		}
	}()

	client := daemonclient.New(controlURL, sup.Restarts(), logger)
	go client.Run(ctx)

	// ctx is also the parent context for every feature's stream hubs, so they shut down
	// alongside everything else on process exit.
	reportingSvc := reportingsvc.New(client, ctx, logger)
	scanningSvc := scanningsvc.New(client, ctx, logger)

	go runRearm(ctx, client, reportingSvc, scanningSvc, logger)

	structureFiles, err := structurefiles.New(cfg.StructureFileDir)
	if err != nil {
		logger.Error("failed to initialize structure file storage", "error", err)
		os.Exit(1)
	}

	api := rest.New(reportingSvc, scanningSvc, sup, client, structureFiles, logger)
	mux := rest.Router(api)
	ws.RegisterRoutes(mux, ws.New(reportingSvc, scanningSvc, logger))

	httpServer := &http.Server{Addr: cfg.HTTPAddr, Handler: mux}
	serverErrCh := make(chan error, 1)
	go func() {
		logger.Info("http server listening", "addr", cfg.HTTPAddr)
		serverErrCh <- httpServer.ListenAndServe()
	}()

	select {
	case <-ctx.Done():
		logger.Info("shutdown signal received")
	case err := <-serverErrCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("http server failed", "error", err)
		}
		stop()
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownGrace)
	defer cancel()
	if err := httpServer.Shutdown(shutdownCtx); err != nil {
		logger.Warn("http server did not shut down cleanly", "error", err)
	}

	<-supervisorDone
}

// runRearm watches for control-channel reconnects and, on every one after the initial
// startup connection, replays every device/scan that was active immediately before the
// disconnect through the (now fresh) daemon process, via each feature's own service. The
// service's Snapshot/Clear/Start are the only entry points used here — main never reaches
// into a feature's data layer directly.
func runRearm(ctx context.Context, client *daemonclient.Client, reportingSvc *reportingsvc.Service, scanningSvc *scanningsvc.Service, logger *slog.Logger) {
	first := true
	for {
		select {
		case <-ctx.Done():
			return
		case <-client.Reconnects():
			if first {
				first = false
				continue
			}
			rearm(ctx, reportingSvc, scanningSvc, logger)
		}
	}
}

func rearm(ctx context.Context, reportingSvc *reportingsvc.Service, scanningSvc *scanningsvc.Service, logger *slog.Logger) {
	logger.Warn("daemon reconnected after unexpected restart, re-arming active devices/scans")

	deviceParams := reportingSvc.Snapshot()
	reportingSvc.Clear()
	for _, params := range deviceParams {
		device, err := reportingSvc.Start(ctx, params)
		if err != nil {
			logger.Error("re-arm: failed to restart device reporting", "host", params.Host, "error", err)
			continue
		}
		logger.Info("re-arm: device reporting restarted", "host", params.Host, "deviceId", device.ID)
	}

	scanParams := scanningSvc.Snapshot()
	scanningSvc.Clear()
	for _, params := range scanParams {
		scan, err := scanningSvc.Start(ctx, params)
		if err != nil {
			logger.Error("re-arm: failed to restart scan", "interfaceId", params.InterfaceID, "error", err)
			continue
		}
		logger.Info("re-arm: scan restarted", "interfaceId", params.InterfaceID, "scanId", scan.ID)
	}
}
