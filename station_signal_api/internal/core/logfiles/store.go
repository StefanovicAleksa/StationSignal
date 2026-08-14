// Package logfiles empties the on-box log files this stack writes, so an operator can start an
// important session — a substation test run, typically — with a clean capture that contains only
// that session's traffic.
//
// It exists because nothing else reclaims these files: there is deliberately no logrotate in front
// of them (deploy/setup.sh, deploy/README.md both say so), so /var/log/station_signal only ever
// grows, and a capture taken after a day of bench work has to be separated from the run that
// actually matters by reading timestamps by hand.
//
// # Truncate, never unlink
//
// This is the load-bearing property of the whole package. The daemon opens each log lazily with
// fopen(path, "a") and caches the FILE* for its entire process lifetime, with no reopen path
// anywhere (station_signal_daemon/src/log.h). Because that header is `static inline`, there is one
// handle per translation unit rather than per feature — mms_dataset_manager alone has four live
// handles on one file. systemd holds a fifth kind of handle on station-signal-api.log via
// StandardOutput=append:.
//
// Deleting a log would leave every one of those writers appending into an orphaned inode: logging
// silently stops for the rest of the process's life, the disk space is never reclaimed, and
// nothing anywhere reports an error. Truncation is safe precisely because every writer opened in
// append mode — O_APPEND repositions each write to the current end of file, so the next line lands
// at offset 0 with no sparse run of NUL bytes in front of it, and the daemon keeps logging into
// the same file without noticing.
package logfiles

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

// logGlob matches every log this stack writes into its log directory: the daemon's per-feature
// files (station-signal-<feature>.log, one per SS_LOG_FEATURE) and the API's own
// station-signal-api.log, which systemd creates by redirecting this process's stdout/stderr.
//
// A glob rather than a hardcoded list of features, because the feature set is not the whole story:
// which files exist depends on log level and on which code paths actually ran, and real capture
// directories also carry leftovers from removed sinks (a zero-byte station-signal-goose.log
// outlived the one-off SS_LOG_GOOSE sink it came from).
const logGlob = "station-signal-*.log"

// StandardDir is where the daemon writes when STATION_SIGNAL_LOG_DIR is unset — its compiled-in
// default (log.h) and the directory deploy/setup.sh creates. The privileged helper below hardcodes
// this same path, so the helper is only ever useful when the configured directory matches it.
const StandardDir = "/var/log/station_signal"

// helperTimeout bounds one privileged-helper invocation. The helper only truncates a handful of
// files, so this is generous; it exists so a wedged sudo can't pin an HTTP handler forever.
const helperTimeout = 20 * time.Second

// Result reports what a Clear actually did. Partial success is normal and is reported here rather
// than as an error — see Clear.
type Result struct {
	// Dir is the directory that was cleared, echoed back so the caller can show which one it was.
	Dir string `json:"logDir"`
	// ClearedCount is how many files were successfully emptied.
	ClearedCount int `json:"clearedCount"`
	// SkippedCount is how many matched but could not be emptied (almost always a permissions
	// problem on a file this process does not own).
	SkippedCount int `json:"skippedCount"`
	// BytesFreed is the total size of the files that were emptied, before emptying them. This is
	// the number that tells an operator the wipe actually happened.
	BytesFreed int64 `json:"bytesFreed"`
}

// Store empties the log files under a single directory.
type Store struct {
	dir string
	// helperPath is an optional privileged fallback (see Clear). Empty disables it.
	helperPath string
}

// New returns a Store rooted at dir. Unlike structurefiles.New it deliberately does not create the
// directory: this package only ever empties files that already exist, and a missing log directory
// is a legitimate "nothing has been logged yet" state, not something to materialize.
func New(dir string) *Store {
	return &Store{dir: dir}
}

// WithPrivilegedHelper returns a copy of s that falls back to the privileged helper script at
// helperPath for files it cannot truncate itself.
//
// This is needed for exactly one file. On a deployed box the API runs unprivileged as
// `station-signal`, but station-signal-api.log is created by systemd — that is, by PID 1 as root —
// via StandardOutput=append:, so it ends up root-owned and this process cannot truncate it. Left
// alone it would survive the wipe and carry the previous session's noise into the next capture,
// which defeats the point of clearing at all.
func (s *Store) WithPrivilegedHelper(helperPath string) *Store {
	clone := *s
	clone.helperPath = helperPath
	return &clone
}

// Clear empties every log file in the store's directory and reports what it managed to empty.
//
// Individual failures never abort the pass — a file this process cannot touch is counted as
// skipped and the rest are still cleared, mirroring structurefiles.Sweep's "return a count plus
// the first error" shape. A caller showing the result should treat SkippedCount > 0 as information
// to display, not as a failed request: clearing eleven of twelve logs is far more useful than
// refusing to clear any.
//
// A missing directory is not an error either; it means nothing has been logged yet.
func (s *Store) Clear() (Result, error) {
	result := Result{Dir: s.dir}

	paths, err := filepath.Glob(filepath.Join(s.dir, logGlob))
	if err != nil {
		// Only ever ErrBadPattern, and logGlob is a constant — so this is unreachable short of
		// someone editing that constant into something malformed.
		return result, fmt.Errorf("match log files in %q: %w", s.dir, err)
	}

	var firstErr error
	needPrivilege := false

	for _, path := range paths {
		if !s.owns(path) {
			continue
		}
		cleared, size, err := truncate(path)
		switch {
		case cleared:
			result.ClearedCount++
			result.BytesFreed += size
		case os.IsPermission(err):
			// Deliberately not recorded as firstErr yet — the helper below may still clear it, and
			// reporting a failure for a file that then gets cleared would be a lie.
			needPrivilege = true
		case os.IsNotExist(err):
			// Vanished between the glob and here. Already empty in the only sense that matters.
		default:
			result.SkippedCount++
			if firstErr == nil {
				firstErr = err
			}
		}
	}

	if !needPrivilege {
		return result, firstErr
	}

	// Second pass, for the root-owned files only. The helper takes no arguments and hardcodes its
	// own directory, so nothing from here reaches a privileged context; all we can do is ask it to
	// run and then re-measure. If it isn't available (the normal case on a dev laptop, where
	// run_dev.sh already runs this process as root and the first pass therefore succeeded), the
	// remaining files are simply reported as skipped.
	if s.helperPath != "" && s.dir == StandardDir {
		if err := s.runHelper(); err != nil && firstErr == nil {
			firstErr = err
		}
	}

	for _, path := range paths {
		if !s.owns(path) {
			continue
		}
		info, err := os.Stat(path)
		if err != nil {
			continue
		}
		if info.Size() == 0 {
			// Either we cleared it in the first pass (already counted) or the helper just did.
			continue
		}
		result.SkippedCount++
	}

	return result, firstErr
}

// truncate empties one file, reporting the size it had beforehand. The stat is what makes
// BytesFreed meaningful, and doing it here keeps Clear's own loop readable.
func truncate(path string) (cleared bool, size int64, err error) {
	info, err := os.Stat(path)
	if err != nil {
		return false, 0, err
	}
	if err := os.Truncate(path, 0); err != nil {
		return false, 0, err
	}
	return true, info.Size(), nil
}

func (s *Store) runHelper() error {
	ctx, cancel := context.WithTimeout(context.Background(), helperTimeout)
	defer cancel()

	cmd := exec.CommandContext(ctx, "sudo", s.helperPath)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s: %w: %s", filepath.Base(s.helperPath), err, out)
	}
	return nil
}

// owns reports whether path is a file directly inside this store's directory — the same guard
// structurefiles.Store.owns applies, for the same reason: nothing outside the configured directory
// is ever written to, however the path got here.
func (s *Store) owns(path string) bool {
	if path == "" {
		return false
	}
	dir, err := filepath.Abs(s.dir)
	if err != nil {
		return false
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return false
	}
	return filepath.Dir(abs) == dir
}
