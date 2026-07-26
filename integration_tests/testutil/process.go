//go:build integration

package testutil

import (
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

// DaemonPID finds the real ied_reporter_daemon process's PID — the API execs it as a direct
// child (no intermediate shell), so `pgrep -P <api pid>` finds exactly it. Retries briefly:
// two ied_reporter_daemon processes sequentially reusing the same fixed control-channel port
// (127.0.0.1:8767, not configurable) can hit a transient EADDRINUSE while the OS finishes
// releasing the previous one's socket, which the supervisor's own backoff/retry already
// handles — but it means there can be a brief window with no live daemon child at all.
func DaemonPID(t *testing.T, h *Harness) int {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	var lastErr error
	for time.Now().Before(deadline) {
		out, err := exec.Command("pgrep", "-P", strconv.Itoa(h.PID())).Output()
		if err == nil {
			line := strings.TrimSpace(strings.SplitN(strings.TrimSpace(string(out)), "\n", 2)[0])
			if line != "" {
				if pid, convErr := strconv.Atoi(line); convErr == nil {
					return pid
				}
			}
		}
		lastErr = err
		time.Sleep(100 * time.Millisecond)
	}
	require.Failf(t, "failed to find daemon child process", "API pid %d, last pgrep error: %v", h.PID(), lastErr)
	return 0
}
