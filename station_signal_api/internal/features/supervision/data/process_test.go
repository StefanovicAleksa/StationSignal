package data

import (
	"log/slog"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestSpawn_ExitsCleanlyForAQuickCommand(t *testing.T) {
	_, exitCh, err := Spawn("/usr/bin/true", "info", slog.Default())
	require.NoError(t, err)

	select {
	case exitErr := <-exitCh:
		assert.NoError(t, exitErr)
	case <-time.After(2 * time.Second):
		t.Fatal("process did not exit in time")
	}
}

func TestSpawn_NonExistentBinaryReturnsError(t *testing.T) {
	_, _, err := Spawn("/this/path/does/not/exist", "info", slog.Default())

	assert.Error(t, err)
}

func TestProcess_Terminate_GracefulProcessExitsPromptly(t *testing.T) {
	proc, exitCh, err := Spawn("testdata/graceful.sh", "info", slog.Default())
	require.NoError(t, err)
	time.Sleep(100 * time.Millisecond) // let the fixture's trap register before signaling

	proc.Terminate()

	select {
	case <-exitCh:
	case <-time.After(3 * time.Second):
		t.Fatal("process did not exit after Terminate()")
	}
}

func TestProcess_Kill_StubbornProcessIsForcedToExit(t *testing.T) {
	proc, exitCh, err := Spawn("testdata/stubborn.sh", "info", slog.Default())
	require.NoError(t, err)
	time.Sleep(100 * time.Millisecond) // let the fixture's trap register before signaling

	proc.Terminate()

	// The stubborn fixture traps and ignores the interrupt, so it must NOT exit shortly
	// after Terminate() alone.
	select {
	case <-exitCh:
		t.Fatal("stubborn process exited after Terminate() alone — fixture isn't ignoring the signal as expected")
	case <-time.After(300 * time.Millisecond):
	}

	proc.Kill()

	select {
	case <-exitCh:
	case <-time.After(3 * time.Second):
		t.Fatal("process did not exit after Kill()")
	}
}

// The daemon reads STATION_SIGNAL_LOG_LEVEL once at startup and has no other configuration
// input, so this hand-off is the only thing keeping its verbosity in step with the API's mode.
// It must be set explicitly rather than inherited: run_dev.sh launches this process under sudo,
// which scrubs the environment.
func TestSpawn_ExportsTheDaemonLogLevelToTheChild(t *testing.T) {
	dir := t.TempDir()
	script := filepath.Join(dir, "echo-level.sh")
	require.NoError(t, os.WriteFile(script,
		[]byte("#!/bin/sh\nprintf '%s' \"$STATION_SIGNAL_LOG_LEVEL\" > \"$(dirname \"$0\")/level.txt\"\n"), 0o755))

	_, exitCh, err := Spawn(script, "debug", slog.Default())
	require.NoError(t, err)

	select {
	case exitErr := <-exitCh:
		require.NoError(t, exitErr)
	case <-time.After(2 * time.Second):
		t.Fatal("process did not exit in time")
	}

	written, err := os.ReadFile(filepath.Join(dir, "level.txt"))
	require.NoError(t, err)
	assert.Equal(t, "debug", string(written))
}

func TestProcess_TerminateAndKill_NilSafeOnZeroValue(t *testing.T) {
	var proc *Process

	assert.NotPanics(t, proc.Terminate)
	assert.NotPanics(t, proc.Kill)

	zero := &Process{}
	assert.NotPanics(t, zero.Terminate)
	assert.NotPanics(t, zero.Kill)
}
