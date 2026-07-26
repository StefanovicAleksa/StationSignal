// Package structurefiles stores SCL/ICD/CID structure files the frontend uploads for use as a
// reporting device's sclFilePath. This API and the station_signal_daemon it supervises always run
// on the same box (see station_signal_api/CLAUDE.md's single-box deployment model), so a path
// returned here is one the daemon can read directly from its own filesystem.
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
)

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
