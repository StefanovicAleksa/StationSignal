package data

import (
	"context"
	"net"
	"time"
)

// PollReady polls addr until it accepts a TCP connection, or gives up after timeout or ctx
// cancellation.
func PollReady(ctx context.Context, addr string, pollEvery, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, pollEvery)
		if err == nil {
			conn.Close()
			return true
		}
		if !sleepOrDone(ctx, pollEvery) {
			return false
		}
	}
	return false
}

func sleepOrDone(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-t.C:
		return true
	case <-ctx.Done():
		return false
	}
}
