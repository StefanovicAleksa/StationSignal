package config

import (
	"log/slog"
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func fakeBinary(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "fake-daemon")
	require.NoError(t, os.WriteFile(path, []byte("#!/bin/sh\n"), 0o755))
	return path
}

func TestLoad_FlagsTakePrecedenceOverEnv(t *testing.T) {
	flagBin := fakeBinary(t)
	envBin := fakeBinary(t)
	t.Setenv("STATION_SIGNAL_DAEMON_BIN", envBin)
	t.Setenv("STATION_SIGNAL_API_HTTP_ADDR", ":9999")
	t.Setenv("STATION_SIGNAL_API_LOG_LEVEL", "debug")

	cfg, err := Load([]string{"-daemon-bin", flagBin, "-http-addr", ":8080", "-log-level", "warn"})

	require.NoError(t, err)
	assert.Equal(t, flagBin, cfg.DaemonBinPath)
	assert.Equal(t, ":8080", cfg.HTTPAddr)
	assert.Equal(t, "warn", cfg.LogLevel)
}

func TestLoad_FallsBackToEnvWhenFlagsAbsent(t *testing.T) {
	envBin := fakeBinary(t)
	t.Setenv("STATION_SIGNAL_DAEMON_BIN", envBin)
	t.Setenv("STATION_SIGNAL_API_HTTP_ADDR", ":9999")
	t.Setenv("STATION_SIGNAL_API_LOG_LEVEL", "debug")

	cfg, err := Load(nil)

	require.NoError(t, err)
	assert.Equal(t, envBin, cfg.DaemonBinPath)
	assert.Equal(t, ":9999", cfg.HTTPAddr)
	assert.Equal(t, "debug", cfg.LogLevel)
}

func TestLoad_DefaultsWhenNeitherFlagNorEnvSet(t *testing.T) {
	bin := fakeBinary(t)

	cfg, err := Load([]string{"-daemon-bin", bin})

	require.NoError(t, err)
	assert.Equal(t, ":8080", cfg.HTTPAddr)
	assert.Equal(t, ModeProd, cfg.Mode)
	assert.Equal(t, "info", cfg.LogLevel)
	assert.NotEmpty(t, cfg.StructureFileDir)
}

// The mode is the only knob deploy/setup.sh sets; the level it implies is what actually silences
// (or restores) debug logging in this process and in the daemon it spawns.
func TestLoad_ModeDerivesTheLogLevel(t *testing.T) {
	bin := fakeBinary(t)

	tests := []struct {
		args      []string
		wantMode  Mode
		wantLevel string
		wantSlog  slog.Level
	}{
		{[]string{"-mode", "dev"}, ModeDev, "debug", slog.LevelDebug},
		{[]string{"-mode", "prod"}, ModeProd, "info", slog.LevelInfo},
	}

	for _, tt := range tests {
		t.Run(string(tt.wantMode), func(t *testing.T) {
			cfg, err := Load(append([]string{"-daemon-bin", bin}, tt.args...))

			require.NoError(t, err)
			assert.Equal(t, tt.wantMode, cfg.Mode)
			assert.Equal(t, tt.wantLevel, cfg.LogLevel)
			assert.Equal(t, tt.wantSlog, cfg.SlogLevel())
		})
	}
}

func TestLoad_ModeFallsBackToEnv(t *testing.T) {
	bin := fakeBinary(t)
	t.Setenv("STATION_SIGNAL_MODE", "dev")

	cfg, err := Load([]string{"-daemon-bin", bin})

	require.NoError(t, err)
	assert.Equal(t, ModeDev, cfg.Mode)
	assert.Equal(t, "debug", cfg.LogLevel)
}

// The log directory defaults to the daemon's own compiled-in fallback, which is what makes the two
// processes agree when nobody configures anything — and is what internal/core/logfiles then
// empties.
func TestLoad_LogDirDefaultsToTheDaemonsOwnDefault(t *testing.T) {
	bin := fakeBinary(t)

	cfg, err := Load([]string{"-daemon-bin", bin})

	require.NoError(t, err)
	assert.Equal(t, "/var/log/station_signal", cfg.LogDir)
}

// Deliberately the same env var the daemon itself reads, not an API-prefixed one: it names one
// directory both processes use, so setting it once must configure both.
func TestLoad_LogDirFallsBackToTheSharedEnvVar(t *testing.T) {
	bin := fakeBinary(t)
	t.Setenv("STATION_SIGNAL_LOG_DIR", "/tmp/station_signal_logs")

	cfg, err := Load([]string{"-daemon-bin", bin})

	require.NoError(t, err)
	assert.Equal(t, "/tmp/station_signal_logs", cfg.LogDir)
}

func TestLoad_LogDirFlagBeatsEnv(t *testing.T) {
	bin := fakeBinary(t)
	t.Setenv("STATION_SIGNAL_LOG_DIR", "/tmp/from-env")

	cfg, err := Load([]string{"-daemon-bin", bin, "-log-dir", "/tmp/from-flag"})

	require.NoError(t, err)
	assert.Equal(t, "/tmp/from-flag", cfg.LogDir)
}

// An explicit level is the escape hatch for turning one box up or down without changing its mode.
func TestLoad_ExplicitLogLevelOverridesTheModeDefault(t *testing.T) {
	bin := fakeBinary(t)

	cfg, err := Load([]string{"-daemon-bin", bin, "-mode", "dev", "-log-level", "error"})

	require.NoError(t, err)
	assert.Equal(t, ModeDev, cfg.Mode)
	assert.Equal(t, "error", cfg.LogLevel)
	assert.Equal(t, slog.LevelError, cfg.SlogLevel())
}

func TestLoad_RejectsUnknownMode(t *testing.T) {
	bin := fakeBinary(t)

	_, err := Load([]string{"-daemon-bin", bin, "-mode", "staging"})

	require.Error(t, err)
	assert.Contains(t, err.Error(), "staging")
}

// A typo must not be able to turn logging off altogether.
func TestSlogLevel_UnrecognizedLevelFallsBackToInfo(t *testing.T) {
	assert.Equal(t, slog.LevelInfo, Config{LogLevel: "verbose"}.SlogLevel())
}

func TestLoad_StructureFileDirFlagTakesPrecedenceOverEnv(t *testing.T) {
	bin := fakeBinary(t)
	t.Setenv("STATION_SIGNAL_API_STRUCTURE_FILE_DIR", "/env/structure-files")

	cfg, err := Load([]string{"-daemon-bin", bin, "-structure-file-dir", "/flag/structure-files"})

	require.NoError(t, err)
	assert.Equal(t, "/flag/structure-files", cfg.StructureFileDir)
}

func TestLoad_MissingDaemonBinPath(t *testing.T) {
	_, err := Load(nil)

	require.Error(t, err)
	assert.Contains(t, err.Error(), "daemon binary path is required")
}

func TestLoad_DaemonBinPathDoesNotExist(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "does-not-exist")

	_, err := Load([]string{"-daemon-bin", missing})

	require.Error(t, err)
	assert.Contains(t, err.Error(), missing)
}

func TestLoad_InvalidFlagSyntax(t *testing.T) {
	_, err := Load([]string{"-this-flag-does-not-exist"})

	assert.Error(t, err)
}
