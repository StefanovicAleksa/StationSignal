package structurefiles

import (
	"context"
	"log/slog"
	"time"
)

// RunJanitor sweeps the store on a fixed interval until ctx is canceled, asking inUse each time
// for the set of paths currently spoken for. It blocks; call it in a goroutine.
//
// inUse is a callback rather than a value because the answer changes constantly — it is derived
// from whatever devices are running at that moment (see Store.Sweep on why running devices pin
// their file).
func RunJanitor(ctx context.Context, store *Store, inUse func() map[string]bool, logger *slog.Logger) {
	if logger == nil {
		logger = slog.Default()
	}

	ticker := time.NewTicker(SweepEvery)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			removed, err := store.Sweep(inUse(), MaxUnreferencedAge)
			if err != nil {
				// Worth a warning, not a retry: the next tick tries again anyway, and a sweep
				// that cannot delete is a disk filling up quietly otherwise.
				logger.Warn("could not fully sweep uploaded structure files", "error", err)
			}
			if removed > 0 {
				logger.Info("removed unused uploaded structure files", "count", removed)
			}
		}
	}
}
