package config

import (
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
	t.Setenv("IED_REPORTER_DAEMON_BIN", envBin)
	t.Setenv("IED_REPORTER_API_HTTP_ADDR", ":9999")
	t.Setenv("IED_REPORTER_API_LOG_LEVEL", "debug")

	cfg, err := Load([]string{"-daemon-bin", flagBin, "-http-addr", ":8080", "-log-level", "warn"})

	require.NoError(t, err)
	assert.Equal(t, flagBin, cfg.DaemonBinPath)
	assert.Equal(t, ":8080", cfg.HTTPAddr)
	assert.Equal(t, "warn", cfg.LogLevel)
}

func TestLoad_FallsBackToEnvWhenFlagsAbsent(t *testing.T) {
	envBin := fakeBinary(t)
	t.Setenv("IED_REPORTER_DAEMON_BIN", envBin)
	t.Setenv("IED_REPORTER_API_HTTP_ADDR", ":9999")
	t.Setenv("IED_REPORTER_API_LOG_LEVEL", "debug")

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
	assert.Equal(t, "info", cfg.LogLevel)
	assert.NotEmpty(t, cfg.StructureFileDir)
}

func TestLoad_StructureFileDirFlagTakesPrecedenceOverEnv(t *testing.T) {
	bin := fakeBinary(t)
	t.Setenv("IED_REPORTER_API_STRUCTURE_FILE_DIR", "/env/structure-files")

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
