// Package config parses this process's own startup configuration — the daemon binary it
// supervises, its own HTTP listen address, and log verbosity. It has nothing to do with the
// daemon's wire contract (see daemonproto for that).
package config

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
)

// Config holds ied_reporter_api's startup configuration.
type Config struct {
	// DaemonBinPath is the path to an already-built ied_reporter_daemon binary. This process
	// never builds the daemon itself — see ied_reporter_api/CLAUDE.md's "Build vs. prebuilt
	// binary" decision.
	DaemonBinPath string

	// HTTPAddr is the address this API's own HTTP server listens on, e.g. ":8080".
	HTTPAddr string

	// LogLevel is one of "debug", "info", "warn", "error".
	LogLevel string

	// StructureFileDir is the directory (on this process's own host) where uploaded
	// SCL/ICD/CID structure files are stored — see internal/core/structurefiles.
	StructureFileDir string
}

// Load parses configuration from command-line flags, falling back to environment variables,
// falling back to defaults. Flags take precedence over env vars.
func Load(args []string) (Config, error) {
	fs := flag.NewFlagSet("ied_reporter_api", flag.ContinueOnError)

	daemonBinPath := fs.String("daemon-bin", envOrDefault("IED_REPORTER_DAEMON_BIN", ""),
		"path to the prebuilt ied_reporter_daemon binary (required)")
	httpAddr := fs.String("http-addr", envOrDefault("IED_REPORTER_API_HTTP_ADDR", ":8080"),
		"address for this API's own HTTP server to listen on")
	logLevel := fs.String("log-level", envOrDefault("IED_REPORTER_API_LOG_LEVEL", "info"),
		"log level: debug, info, warn, error")
	structureFileDir := fs.String("structure-file-dir", envOrDefault("IED_REPORTER_API_STRUCTURE_FILE_DIR", defaultStructureFileDir()),
		"directory where uploaded SCL/ICD/CID structure files are stored")

	if err := fs.Parse(args); err != nil {
		return Config{}, err
	}

	cfg := Config{
		DaemonBinPath:    *daemonBinPath,
		HTTPAddr:         *httpAddr,
		LogLevel:         *logLevel,
		StructureFileDir: *structureFileDir,
	}

	if cfg.DaemonBinPath == "" {
		return Config{}, fmt.Errorf("daemon binary path is required: set -daemon-bin or IED_REPORTER_DAEMON_BIN")
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

func defaultStructureFileDir() string {
	return filepath.Join(os.TempDir(), "ied_reporter_api", "structure_files")
}
