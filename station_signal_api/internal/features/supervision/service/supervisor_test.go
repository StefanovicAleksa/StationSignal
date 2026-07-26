package service

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/features/supervision/data"
)

// fakeSpawn builds a spawnFn that, on its i-th call, returns spawnErrs[i] if set, otherwise
// a harmless &data.Process{} zero value paired with exitChs[i] (or a fresh channel if there
// aren't enough). data.Process's Terminate/Kill are already nil-safe on the zero value, so
// this stand-in process handle is safe for Run() to call them on — real signal delivery is
// covered by supervision/data's own tests against real fixture processes.
func fakeSpawn(t *testing.T, spawnErrs []error, exitChs []chan error) (spawnFn func(string, *slog.Logger) (*data.Process, <-chan error, error), calls func() int) {
	t.Helper()
	var n int32
	fn := func(string, *slog.Logger) (*data.Process, <-chan error, error) {
		i := int(atomic.AddInt32(&n, 1)) - 1
		if i < len(spawnErrs) && spawnErrs[i] != nil {
			return nil, nil, spawnErrs[i]
		}
		var ch chan error
		if i < len(exitChs) {
			ch = exitChs[i]
		} else {
			ch = make(chan error, 1)
		}
		return &data.Process{}, ch, nil
	}
	return fn, func() int { return int(atomic.LoadInt32(&n)) }
}

func alwaysReady(context.Context, string, time.Duration, time.Duration) bool { return true }

func newTestSupervisor(spawnFn func(string, *slog.Logger) (*data.Process, <-chan error, error), pollReadyFn func(context.Context, string, time.Duration, time.Duration) bool) *Supervisor {
	return &Supervisor{
		binPath:     "unused",
		controlAddr: "unused",
		logger:      slog.Default(),
		spawnFn:     spawnFn,
		pollReadyFn: pollReadyFn,
		restarts:    make(chan struct{}, 1),
	}
}

func waitRestart(t *testing.T, s *Supervisor) {
	t.Helper()
	select {
	case <-s.Restarts():
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for Restarts() signal")
	}
}

// autoExitOnDone feeds exitCh once ctx is canceled, simulating a well-behaved process that
// exits shortly after being asked to. Terminate()/Kill() on the zero-value fake Process are
// no-ops, so without this a test that cancels ctx would otherwise leave Run() blocked
// forever waiting on an exitCh nothing ever feeds — this keeps every test's goroutine from
// leaking past the test. Exact terminate/grace-period/kill signal timing is covered by
// supervision/data's own tests against real processes, not here.
func autoExitOnDone(ctx context.Context, exitCh chan error) {
	var once sync.Once
	go func() {
		<-ctx.Done()
		once.Do(func() { exitCh <- nil })
	}()
}

func TestSupervisor_Run_SignalsReadyAndSetsRunning(t *testing.T) {
	exitCh := make(chan error, 1)
	spawnFn, _ := fakeSpawn(t, nil, []chan error{exitCh})
	s := newTestSupervisor(spawnFn, alwaysReady)

	ctx, cancel := context.WithCancel(context.Background())
	autoExitOnDone(ctx, exitCh)
	done := make(chan struct{})
	go func() { s.Run(ctx); close(done) }()

	waitRestart(t, s)
	assert.True(t, s.Running())

	cancel()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Run did not return after ctx cancellation")
	}
	assert.False(t, s.Running())
}

func TestSupervisor_Run_RetriesAfterSpawnFailure(t *testing.T) {
	exitCh := make(chan error, 1)
	spawnFn, calls := fakeSpawn(t, []error{errors.New("boom"), nil}, []chan error{nil, exitCh})
	s := newTestSupervisor(spawnFn, alwaysReady)

	ctx, cancel := context.WithCancel(context.Background())
	autoExitOnDone(ctx, exitCh)
	defer cancel()
	go s.Run(ctx)

	waitRestart(t, s)
	assert.GreaterOrEqual(t, calls(), 2, "should have retried spawn after the first failure")
	assert.True(t, s.Running())
}

func TestSupervisor_Run_RestartsAfterUnexpectedExit(t *testing.T) {
	firstExit := make(chan error, 1)
	secondExit := make(chan error, 1)
	spawnFn, calls := fakeSpawn(t, nil, []chan error{firstExit, secondExit})
	s := newTestSupervisor(spawnFn, alwaysReady)

	ctx, cancel := context.WithCancel(context.Background())
	autoExitOnDone(ctx, secondExit)
	defer cancel()
	go s.Run(ctx)

	waitRestart(t, s)
	assert.Equal(t, 1, calls())

	firstExit <- errors.New("exit status 1")

	waitRestart(t, s)
	assert.Equal(t, 2, calls())
	assert.True(t, s.Running())
}

func TestSupervisor_Run_NotReadyStillWaitsForExit(t *testing.T) {
	exitCh := make(chan error, 1)
	spawnFn, _ := fakeSpawn(t, nil, []chan error{exitCh})
	neverReady := func(context.Context, string, time.Duration, time.Duration) bool { return false }
	s := newTestSupervisor(spawnFn, neverReady)

	ctx, cancel := context.WithCancel(context.Background())
	autoExitOnDone(ctx, exitCh)
	defer cancel()
	go s.Run(ctx)

	// No Restarts() signal should fire since readiness never succeeded.
	select {
	case <-s.Restarts():
		t.Fatal("Restarts() should not fire when the process never became ready")
	case <-time.After(200 * time.Millisecond):
	}
	assert.True(t, s.Running(), "process is still considered running even though it never became ready")
}

func TestSupervisor_Restarts_BufferedSignalDoesNotBlockSender(t *testing.T) {
	exitCh := make(chan error, 1)
	spawnFn, _ := fakeSpawn(t, nil, []chan error{exitCh})
	s := newTestSupervisor(spawnFn, alwaysReady)

	ctx, cancel := context.WithCancel(context.Background())
	autoExitOnDone(ctx, exitCh)
	defer cancel()
	go s.Run(ctx)

	// Give Run a moment to signal Restarts() once, which nobody drains — signalRestart()
	// must not block on a full buffered channel.
	time.Sleep(100 * time.Millisecond)
	assert.True(t, s.Running())
}

func TestSupervisor_Running_FalseBeforeAnySpawn(t *testing.T) {
	s := newTestSupervisor(func(string, *slog.Logger) (*data.Process, <-chan error, error) {
		return nil, nil, errors.New("never called in this test")
	}, alwaysReady)

	require.False(t, s.Running())
}
