// Package domain holds supervision's pure vocabulary: restart-backoff tuning and the pure
// backoff policy function. No I/O, no imports of this feature's data/service layers.
package domain

import "time"

// Tuning constants for the supervision service's restart loop.
const (
	InitialBackoff = 200 * time.Millisecond
	MaxBackoff     = 5 * time.Second
	ReadyPollEvery = 50 * time.Millisecond
	ReadyTimeout   = 10 * time.Second
	// TermGracePeriod bounds how long Run waits after SIGTERM before escalating to SIGKILL.
	// Graceful daemon shutdown tears down every active device's MMS connection first
	// (station_signal_daemon's MmsReportClientConnection_stop — deletes its own dynamic
	// datasets, then a real ACSE Release), each a real wire round-trip against a possibly
	// slow or already-struggling real IED, not just local cleanup. A SIGKILL that lands
	// before that finishes skips it entirely, leaving the IED holding an un-released MMS
	// association — the daemon's own CLAUDE.md documents this as "the ungraceful-restart
	// gap" its cleanup can't reach. 8s gives real-device teardown realistic room; a daemon
	// with no active devices still exits almost immediately, so this isn't paid on the
	// common path.
	TermGracePeriod = 8 * time.Second
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
