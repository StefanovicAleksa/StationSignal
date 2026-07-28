// Package config parses this process's own startup configuration — the daemon binary it
// supervises, its own HTTP listen address, and log verbosity. It has nothing to do with the
// daemon's wire contract (see daemonproto for that).
package config

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
)

// Config holds station_signal_api's startup configuration.
type Config struct {
	// DaemonBinPath is the path to an already-built station_signal_daemon binary. This process
	// never builds the daemon itself — see station_signal_api/CLAUDE.md's "Build vs. prebuilt
	// binary" decision.
	DaemonBinPath string

	// HTTPAddr is the address this API's own HTTP server listens on, e.g. ":8080".
	HTTPAddr string

	// LogLevel is one of "debug", "info", "warn", "error".
	LogLevel string

	// StructureFileDir is the directory (on this process's own host) where uploaded
	// SCL/ICD/CID structure files are stored — see internal/core/structurefiles.
	StructureFileDir string

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
	logLevel := fs.String("log-level", envOrDefault("STATION_SIGNAL_API_LOG_LEVEL", "info"),
		"log level: debug, info, warn, error")
	structureFileDir := fs.String("structure-file-dir", envOrDefault("STATION_SIGNAL_API_STRUCTURE_FILE_DIR", defaultStructureFileDir()),
		"directory where uploaded SCL/ICD/CID structure files are stored")
	netconfigHelperPath := fs.String("netconfig-helper", envOrDefault("STATION_SIGNAL_API_NETCONFIG_HELPER", "/opt/station_signal/bin/station-signal-netconfig.sh"),
		"path to the privileged network-config helper script (invoked via sudo)")
	netconfigRevertTimeoutSeconds := fs.Int("netconfig-revert-timeout-seconds", envIntOrDefault("STATION_SIGNAL_API_NETCONFIG_REVERT_TIMEOUT_SECONDS", 90),
		"seconds a provisional network change waits for confirmation before auto-reverting")

	if err := fs.Parse(args); err != nil {
		return Config{}, err
	}

	cfg := Config{
		DaemonBinPath:                 *daemonBinPath,
		HTTPAddr:                      *httpAddr,
		LogLevel:                      *logLevel,
		StructureFileDir:              *structureFileDir,
		NetconfigHelperPath:           *netconfigHelperPath,
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
