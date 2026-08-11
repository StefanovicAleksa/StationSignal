// Package structurefiles stores SCL/ICD/CID structure files the frontend uploads for use as a
// reporting device's sclFilePath. This API and the station_signal_daemon it supervises always run
// on the same box (see station_signal_api/CLAUDE.md's single-box deployment model), so a path
// returned here is one the daemon can read directly from its own filesystem.
//
// Storage is deliberately temporary. A real station SCD runs to tens of megabytes and the target
// box is a Raspberry Pi with a fixed, small disk, so nothing uploaded here is kept once it has
// stopped being useful — see Sweep for the exact retention rule and why anything has to be kept
// at all.
package structurefiles

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// MaxUnreferencedAge is how long an uploaded file survives once nothing is using it. It only has
// to cover the gap between the upload and the POST /api/devices that names it — the daemon parses
// the file into memory during that one call and never reads it again ("it has zero purpose
// afterward", per the daemon's own orchestration_api.c) — so this is generous already.
const MaxUnreferencedAge = 30 * time.Minute

// SweepEvery is how often the janitor looks. Retention is therefore MaxUnreferencedAge plus at
// most one interval.
const SweepEvery = 5 * time.Minute

// ErrUnsupportedExtension is returned by Save when the uploaded file's extension isn't one of
// the recognized SCL structure-file formats.
var ErrUnsupportedExtension = errors.New("unsupported file type: expected .icd, .cid, .scd, or .xml")

var allowedExtensions = map[string]bool{
	".icd": true,
	".cid": true,
	".scd": true,
	".xml": true,
}

// Store saves uploaded structure files under a single directory on disk.
type Store struct {
	dir string
}

// New creates the storage directory (if it doesn't already exist) and returns a Store rooted
// there.
func New(dir string) (*Store, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("create structure file directory %q: %w", dir, err)
	}
	return &Store{dir: dir}, nil
}

// Save writes r to a new file in the store's directory and returns its absolute path. The
// original filename is only used for its extension and as a readability hint in the stored
// name — a random component is always prepended so concurrent uploads and repeated filenames
// never collide or overwrite each other, and any directory components in originalName are
// discarded so it can never escape the store's directory.
func (s *Store) Save(originalName string, r io.Reader) (string, error) {
	ext := strings.ToLower(filepath.Ext(originalName))
	if !allowedExtensions[ext] {
		return "", ErrUnsupportedExtension
	}

	base := sanitizeBase(strings.TrimSuffix(filepath.Base(originalName), filepath.Ext(originalName)))
	unique, err := randomHex(8)
	if err != nil {
		return "", fmt.Errorf("generate unique filename: %w", err)
	}

	path := filepath.Join(s.dir, unique+"-"+base+ext)

	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		return "", fmt.Errorf("create structure file: %w", err)
	}
	defer f.Close()

	if _, err := io.Copy(f, r); err != nil {
		os.Remove(path)
		return "", fmt.Errorf("write structure file: %w", err)
	}

	return path, nil
}

// Touch resets a stored file's age, so an upload that is actively being used to start a device
// can't be swept out from under the daemon mid-start. A path outside this store's directory is
// silently ignored rather than treated as an error: sclFilePath is allowed to name any file on
// the daemon's own filesystem, not just one this store wrote.
func (s *Store) Touch(path string) {
	if !s.owns(path) {
		return
	}
	now := time.Now()
	_ = os.Chtimes(path, now, now)
}

// Sweep deletes every file in the store's directory that is both older than maxAge and absent
// from inUse, returning how many it removed.
//
// This is the whole retention policy, expressed as one rule with no lifecycle hooks to get wrong:
// it covers a file uploaded and never used, a start that failed, and a device that has since been
// stopped, without any of them needing to notify anything.
//
// inUse exists for one reason: crash re-arm. The daemon reads the file only while starting a
// device, but if it dies, the API restarts it and replays every active device's StartParams —
// sclFilePath included — so a file stays alive as long as some running device could need
// replaying. That bounds usage by the number of devices actually being watched, not by how many
// files have ever been uploaded. Passing a nil inUse (nothing can be running) with a zero maxAge
// therefore empties the directory, which is exactly what a fresh process wants.
func (s *Store) Sweep(inUse map[string]bool, maxAge time.Duration) (int, error) {
	entries, err := os.ReadDir(s.dir)
	if err != nil {
		return 0, fmt.Errorf("read structure file directory %q: %w", s.dir, err)
	}

	cutoff := time.Now().Add(-maxAge)
	removed := 0
	var firstErr error
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		path := filepath.Join(s.dir, entry.Name())
		if inUse[path] {
			continue
		}
		info, err := entry.Info()
		if err != nil {
			// Vanished between ReadDir and here — already gone, which is the goal.
			continue
		}
		if info.ModTime().After(cutoff) {
			continue
		}
		if err := os.Remove(path); err != nil && firstErr == nil {
			firstErr = fmt.Errorf("remove stale structure file %q: %w", path, err)
			continue
		}
		removed++
	}
	return removed, firstErr
}

// owns reports whether path is a file directly inside this store's directory — the guard that
// keeps Touch/Sweep from ever acting on something the store didn't write.
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

func randomHex(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

// sanitizeBase strips originalName down to a short, filesystem-safe stem: only
// alphanumerics/-/_ survive, everything else (including any path separators) becomes '_'.
func sanitizeBase(name string) string {
	name = strings.TrimSpace(name)
	var sb strings.Builder
	for _, r := range name {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-', r == '_':
			sb.WriteRune(r)
		default:
			sb.WriteRune('_')
		}
	}
	result := sb.String()
	if len(result) > 64 {
		result = result[:64]
	}
	if result == "" {
		return "structure"
	}
	return result
}
