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
func Spawn(binPath string, logger *slog.Logger) (*Process, <-chan error, error) {
	cmd := exec.Command(binPath)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr

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
