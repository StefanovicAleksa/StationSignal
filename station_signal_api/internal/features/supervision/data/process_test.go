package data

import (
	"log/slog"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestSpawn_ExitsCleanlyForAQuickCommand(t *testing.T) {
	_, exitCh, err := Spawn("/usr/bin/true", slog.Default())
	require.NoError(t, err)

	select {
	case exitErr := <-exitCh:
		assert.NoError(t, exitErr)
	case <-time.After(2 * time.Second):
		t.Fatal("process did not exit in time")
	}
}

func TestSpawn_NonExistentBinaryReturnsError(t *testing.T) {
	_, _, err := Spawn("/this/path/does/not/exist", slog.Default())

	assert.Error(t, err)
}

func TestProcess_Terminate_GracefulProcessExitsPromptly(t *testing.T) {
	proc, exitCh, err := Spawn("testdata/graceful.sh", slog.Default())
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
	proc, exitCh, err := Spawn("testdata/stubborn.sh", slog.Default())
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

func TestProcess_TerminateAndKill_NilSafeOnZeroValue(t *testing.T) {
	var proc *Process

	assert.NotPanics(t, proc.Terminate)
	assert.NotPanics(t, proc.Kill)

	zero := &Process{}
	assert.NotPanics(t, zero.Terminate)
	assert.NotPanics(t, zero.Kill)
}
