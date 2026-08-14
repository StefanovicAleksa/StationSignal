// Package config parses this process's own startup configuration — the daemon binary it
// supervises, its own HTTP listen address, which deployment mode it runs in, and log verbosity.
// It has nothing to do with the daemon's wire contract (see daemonproto for that).
package config

import (
	"flag"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strconv"

	"station_signal_api/internal/core/logfiles"
)

// Mode is which of the two deployment postures this process runs in. It is the single knob the
// whole stack's verbosity hangs off: deploy/setup.sh takes it as its one argument, persists it to
// /etc/station-signal/station-signal.env, and every process derives its own logging from it.
type Mode string

const (
	// ModeDev is a workstation or an on-site box being actively debugged: debug logging on, plus
	// the per-request HTTP access log.
	ModeDev Mode = "dev"
	// ModeProd is an unattended substation deployment: no debug logging anywhere, no access log.
	ModeProd Mode = "prod"
)

// Config holds station_signal_api's startup configuration.
type Config struct {
	// Mode is "dev" or "prod" — see Mode. Defaults to prod: an install that never says which it
	// wants is a deployment, and the quiet setting is the safe one to guess.
	Mode Mode

	// DaemonBinPath is the path to an already-built station_signal_daemon binary. This process
	// never builds the daemon itself — see station_signal_api/CLAUDE.md's "Build vs. prebuilt
	// binary" decision.
	DaemonBinPath string

	// HTTPAddr is the address this API's own HTTP server listens on, e.g. ":8080".
	HTTPAddr string

	// LogLevel is one of "debug", "info", "warn", "error". Normally left unset and derived from
	// Mode (dev → debug, prod → info); setting it explicitly overrides that, for turning one
	// deployed box up or down without changing what mode it is in.
	LogLevel string

	// StructureFileDir is the directory (on this process's own host) where uploaded
	// SCL/ICD/CID structure files are stored — see internal/core/structurefiles.
	StructureFileDir string

	// LogDir is where this stack's log files live: the daemon's per-feature
	// station-signal-<feature>.log files and, on a deployed box, the station-signal-api.log that
	// systemd writes by redirecting this process's own stdout. Used by internal/core/logfiles to
	// empty them, and exported to the daemon on spawn so the two cannot disagree about which
	// directory that is (see internal/features/supervision/data.Spawn).
	//
	// The default matches the daemon's own compiled-in fallback in src/log.h, which is what makes
	// the two agree when nobody sets anything.
	LogDir string

	// ClearLogsHelperPath is the absolute path to the privileged clear-logs helper script
	// (deploy/scripts/station-signal-clearlogs.sh). Needed for exactly one file: on a deployed box
	// station-signal-api.log is created by systemd as root, so this unprivileged process cannot
	// truncate it itself.
	ClearLogsHelperPath string

	// NetconfigHelperPath is the absolute path to the privileged network-config helper script
	// (deploy/scripts/station-signal-netconfig.sh) this process shells out to via sudo — see
	// internal/features/network/data.ScriptRunner.
	NetconfigHelperPath string

	// NetconfigRevertTimeoutSeconds is how long a provisionally-applied network change waits
	// for confirmation before it's auto-reverted.
	NetconfigRevertTimeoutSeconds int
}

// Load parses configuration from command-line flags, falling back to environment variables,
// falling back to defaults. Flags take precedence over env vars.
func Load(args []string) (Config, error) {
	fs := flag.NewFlagSet("station_signal_api", flag.ContinueOnError)

	daemonBinPath := fs.String("daemon-bin", envOrDefault("STATION_SIGNAL_DAEMON_BIN", ""),
		"path to the prebuilt station_signal_daemon binary (required)")
	httpAddr := fs.String("http-addr", envOrDefault("STATION_SIGNAL_API_HTTP_ADDR", ":8080"),
		"address for this API's own HTTP server to listen on")
	mode := fs.String("mode", envOrDefault("STATION_SIGNAL_MODE", string(ModeProd)),
		"deployment mode: dev (debug logging + HTTP access log) or prod (neither)")
	// Empty means "derive from mode" — the two are resolved together below.
	logLevel := fs.String("log-level", envOrDefault("STATION_SIGNAL_API_LOG_LEVEL", ""),
		"log level: debug, info, warn, error (defaults to debug in dev mode, info in prod)")
	structureFileDir := fs.String("structure-file-dir", envOrDefault("STATION_SIGNAL_API_STRUCTURE_FILE_DIR", defaultStructureFileDir()),
		"directory where uploaded SCL/ICD/CID structure files are stored")
	// Deliberately the same env var the daemon itself reads (src/log.h), not a
	// STATION_SIGNAL_API_-prefixed one: it names a single directory both processes write into, so
	// one variable setting it for both is the point.
	logDir := fs.String("log-dir", envOrDefault("STATION_SIGNAL_LOG_DIR", logfiles.StandardDir),
		"directory holding this stack's log files (shared with the daemon)")
	netconfigHelperPath := fs.String("netconfig-helper", envOrDefault("STATION_SIGNAL_API_NETCONFIG_HELPER", "/opt/station_signal/bin/station-signal-netconfig.sh"),
		"path to the privileged network-config helper script (invoked via sudo)")
	clearLogsHelperPath := fs.String("clear-logs-helper", envOrDefault("STATION_SIGNAL_API_CLEAR_LOGS_HELPER", "/opt/station_signal/bin/station-signal-clearlogs.sh"),
		"path to the privileged clear-logs helper script (invoked via sudo)")
	netconfigRevertTimeoutSeconds := fs.Int("netconfig-revert-timeout-seconds", envIntOrDefault("STATION_SIGNAL_API_NETCONFIG_REVERT_TIMEOUT_SECONDS", 90),
		"seconds a provisional network change waits for confirmation before auto-reverting")

	if err := fs.Parse(args); err != nil {
		return Config{}, err
	}

	resolvedMode := Mode(*mode)
	if resolvedMode != ModeDev && resolvedMode != ModeProd {
		return Config{}, fmt.Errorf("invalid mode %q: expected %q or %q", *mode, ModeDev, ModeProd)
	}

	resolvedLogLevel := *logLevel
	if resolvedLogLevel == "" {
		resolvedLogLevel = "info"
		if resolvedMode == ModeDev {
			resolvedLogLevel = "debug"
		}
	}

	cfg := Config{
		Mode:                          resolvedMode,
		DaemonBinPath:                 *daemonBinPath,
		HTTPAddr:                      *httpAddr,
		LogLevel:                      resolvedLogLevel,
		StructureFileDir:              *structureFileDir,
		LogDir:                        *logDir,
		NetconfigHelperPath:           *netconfigHelperPath,
		ClearLogsHelperPath:           *clearLogsHelperPath,
		NetconfigRevertTimeoutSeconds: *netconfigRevertTimeoutSeconds,
	}

	if cfg.DaemonBinPath == "" {
		return Config{}, fmt.Errorf("daemon binary path is required: set -daemon-bin or STATION_SIGNAL_DAEMON_BIN")
	}
	if _, err := os.Stat(cfg.DaemonBinPath); err != nil {
		return Config{}, fmt.Errorf("daemon binary path %q: %w", cfg.DaemonBinPath, err)
	}

	return cfg, nil
}

// SlogLevel maps LogLevel onto slog's own level type. An unrecognized value falls back to Info
// rather than erroring — same lenient posture as envIntOrDefault, and a typo here must not be
// able to silence logging altogether.
func (c Config) SlogLevel() slog.Level {
	switch c.LogLevel {
	case "debug":
		return slog.LevelDebug
	case "warn":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

func envOrDefault(key, def string) string {
	if v, ok := os.LookupEnv(key); ok {
		return v
	}
	return def
}

func envIntOrDefault(key string, def int) int {
	v, ok := os.LookupEnv(key)
	if !ok {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		return def
	}
	return n
}

func defaultStructureFileDir() string {
	return filepath.Join(os.TempDir(), "station_signal_api", "structure_files")
}
