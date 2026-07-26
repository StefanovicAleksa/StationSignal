// Package domain holds supervision's pure vocabulary: restart-backoff tuning and the pure
// backoff policy function. No I/O, no imports of this feature's data/service layers.
package domain

import "time"

// Tuning constants for the supervision service's restart loop.
const (
	InitialBackoff  = 200 * time.Millisecond
	MaxBackoff      = 5 * time.Second
	ReadyPollEvery  = 50 * time.Millisecond
	ReadyTimeout    = 10 * time.Second
	TermGracePeriod = 3 * time.Second
)

// NextBackoff returns the next backoff delay after a failed spawn or unexpected daemon exit:
// double cur, capped at MaxBackoff.
func NextBackoff(cur time.Duration) time.Duration {
	next := cur * 2
	if next > MaxBackoff {
		return MaxBackoff
	}
	return next
}
