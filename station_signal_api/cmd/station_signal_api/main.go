// Command station_signal_api supervises a single station_signal_daemon process and relays
// control commands and live report/scan data between it and a frontend, over plain REST +
// websocket endpoints. See station_signal_api/CLAUDE.md for the service's scope.
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

	"station_signal_api/internal/controllers/rest"
	"station_signal_api/internal/controllers/ws"
	"station_signal_api/internal/core/config"
	"station_signal_api/internal/core/daemonclient"
	"station_signal_api/internal/core/structurefiles"
	networkdata "station_signal_api/internal/features/network/data"
	networksvc "station_signal_api/internal/features/network/service"
	reportingsvc "station_signal_api/internal/features/reporting/service"
	scanningsvc "station_signal_api/internal/features/scanning/service"
	supervisionsvc "station_signal_api/internal/features/supervision/service"
)

const (
	controlAddr   = "127.0.0.1:8767"
	controlURL    = "ws://" + controlAddr
	shutdownGrace = 5 * time.Second
)

func main() {
	// The level is a LevelVar rather than a fixed option because the logger has to exist before
	// config.Load can run (a config error is itself reported through it), and every downstream
	// constructor closes over this one logger value. Setting the var afterwards re-levels all of
	// them without rebuilding anything.
	logLevel := new(slog.LevelVar)
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: logLevel}))

	cfg, err := config.Load(os.Args[1:])
	if err != nil {
		logger.Error("invalid configuration", "error", err)
		os.Exit(1)
	}
	logLevel.Set(cfg.SlogLevel())

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	sup := supervisionsvc.New(cfg.DaemonBinPath, controlAddr, cfg.LogLevel, logger)
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
	// Nothing can be referencing an upload yet — this process just started, so its device
	// registry is empty and crash re-arm has nothing to replay — which makes startup the one
	// moment the whole directory is provably disposable. Clears anything a kill -9 left behind.
	if removed, err := structureFiles.Sweep(nil, 0); err != nil {
		logger.Warn("could not clear leftover uploaded structure files at startup", "error", err)
	} else if removed > 0 {
		logger.Info("cleared leftover uploaded structure files at startup", "count", removed)
	}
	go structurefiles.RunJanitor(ctx, structureFiles, func() map[string]bool {
		inUse := make(map[string]bool)
		for _, d := range reportingSvc.Snapshot() {
			if d.StartParams.SCLFilePath != "" {
				inUse[d.StartParams.SCLFilePath] = true
			}
		}
		return inUse
	}, logger)

	networkSvc := networksvc.New(
		func() int { return len(reportingSvc.Snapshot()) },
		func() int { return len(scanningSvc.Snapshot()) },
		networkdata.NewScriptRunner(cfg.NetconfigHelperPath),
		networkdata.IPStatusReader{},
		time.Duration(cfg.NetconfigRevertTimeoutSeconds)*time.Second,
		logger,
	)
	// Clear any network change orphaned by a reboot or crash. The helper's auto-revert is a
	// transient systemd timer, which does not survive a reboot — but its on-disk pending marker
	// does, and that marker refuses every subsequent apply. Without this, a box that went down
	// mid-change comes back up permanently unable to change its own address.
	networkSvc.Reconcile(ctx)

	api := rest.New(reportingSvc, scanningSvc, sup, client, structureFiles, networkSvc, logger)
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

	devices := reportingSvc.Snapshot()
	reportingSvc.Clear()
	for _, d := range devices {
		device, err := reportingSvc.Start(ctx, d.SessionID, d.StartParams)
		if err != nil {
			logger.Error("re-arm: failed to restart device reporting", "host", d.StartParams.Host, "error", err)
			continue
		}
		// Debug, not Info: one line per device on every daemon restart. The Warn above already
		// says a re-arm happened at all, and a failure to re-arm is still an Error.
		logger.Debug("re-arm: device reporting restarted", "host", d.StartParams.Host, "deviceId", device.ID)
	}

	scans := scanningSvc.Snapshot()
	scanningSvc.Clear()
	for _, sc := range scans {
		scan, err := scanningSvc.Start(ctx, sc.SessionID, sc.StartParams)
		if err != nil {
			logger.Error("re-arm: failed to restart scan", "interfaceId", sc.StartParams.InterfaceID, "error", err)
			continue
		}
		logger.Debug("re-arm: scan restarted", "interfaceId", sc.StartParams.InterfaceID, "scanId", scan.ID)
	}
}
