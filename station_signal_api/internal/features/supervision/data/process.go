// Package data is supervision's I/O layer: the raw OS process mechanics (spawn, wait,
// terminate, kill) and the TCP readiness probe. It knows the daemon's types (via domain) but
// nothing about restart sequencing — that's service's job.
package data

import (
	"fmt"
	"log/slog"
	"os"
	"os/exec"
)

// Process wraps one running daemon child process.
type Process struct {
	cmd *exec.Cmd
}

// Spawn starts the daemon binary at binPath and returns the Process handle plus a channel
// that receives its exit error (nil for a clean exit) exactly once.
//
// daemonLogLevel and daemonLogDir are exported into the child's environment as
// STATION_SIGNAL_LOG_LEVEL and STATION_SIGNAL_LOG_DIR (the daemon's only two configuration inputs,
// each read once at startup — see its src/log.h), so the daemon's verbosity and log location
// always match this process's own. Set explicitly rather than relying on plain inheritance because
// run_dev.sh launches this process under sudo, which scrubs the environment — the daemon would
// silently fall back to its defaults, and dev mode would produce no debug output from the process
// that emits most of it.
//
// The directory matters beyond tidiness: internal/core/logfiles empties exactly the directory this
// API is configured with, so if the daemon were left to fall back to its own default the two could
// write to and clear different places, and "clear the logs" would quietly leave the real ones
// untouched.
func Spawn(binPath string, daemonLogLevel string, daemonLogDir string, logger *slog.Logger) (*Process, <-chan error, error) {
	cmd := exec.Command(binPath)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Env = append(os.Environ(),
		"STATION_SIGNAL_LOG_LEVEL="+daemonLogLevel,
		"STATION_SIGNAL_LOG_DIR="+daemonLogDir,
	)

	if err := cmd.Start(); err != nil {
		return nil, nil, fmt.Errorf("start daemon: %w", err)
	}

	exitCh := make(chan error, 1)
	go func() {
		exitCh <- cmd.Wait()
	}()

	logger.Info("daemon process started", "pid", cmd.Process.Pid)
	return &Process{cmd: cmd}, exitCh, nil
}

// Terminate sends an interrupt (SIGINT) to the process. It does not wait for exit — the
// caller already owns the exit channel from Spawn, fed by the sole cmd.Wait() goroutine, and
// must select on that (with a timeout, escalating to Kill) instead of waiting here too, since
// a process may only be reaped once.
func (p *Process) Terminate() {
	if p == nil || p.cmd == nil || p.cmd.Process == nil {
		return
	}
	_ = p.cmd.Process.Signal(os.Interrupt)
}

// Kill sends SIGKILL to the process.
func (p *Process) Kill() {
	if p == nil || p.cmd == nil || p.cmd.Process == nil {
		return
	}
	_ = p.cmd.Process.Kill()
}
