package structurefiles

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestNew_CreatesDirectory(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "nested", "structure_files")

	_, err := New(dir)

	require.NoError(t, err)
	info, err := os.Stat(dir)
	require.NoError(t, err)
	assert.True(t, info.IsDir())
}

func TestSave_WritesFileAndReturnsReadablePath(t *testing.T) {
	store, err := New(t.TempDir())
	require.NoError(t, err)

	path, err := store.Save("device.icd", strings.NewReader("<SCL/>"))

	require.NoError(t, err)
	content, err := os.ReadFile(path)
	require.NoError(t, err)
	assert.Equal(t, "<SCL/>", string(content))
	assert.True(t, filepath.IsAbs(path))
}

func TestSave_RejectsUnsupportedExtension(t *testing.T) {
	store, err := New(t.TempDir())
	require.NoError(t, err)

	_, err = store.Save("device.exe", strings.NewReader("data"))

	require.Error(t, err)
	assert.True(t, errors.Is(err, ErrUnsupportedExtension))
}

func TestSave_AcceptsAllRecognizedExtensions(t *testing.T) {
	store, err := New(t.TempDir())
	require.NoError(t, err)

	for _, ext := range []string{".icd", ".cid", ".scd", ".xml", ".ICD"} {
		_, err := store.Save("device"+ext, strings.NewReader("data"))
		assert.NoError(t, err, "extension %s should be accepted", ext)
	}
}

func TestSave_RepeatedFilenamesDoNotCollide(t *testing.T) {
	store, err := New(t.TempDir())
	require.NoError(t, err)

	path1, err := store.Save("device.icd", strings.NewReader("first"))
	require.NoError(t, err)
	path2, err := store.Save("device.icd", strings.NewReader("second"))
	require.NoError(t, err)

	assert.NotEqual(t, path1, path2)
	content1, err := os.ReadFile(path1)
	require.NoError(t, err)
	content2, err := os.ReadFile(path2)
	require.NoError(t, err)
	assert.Equal(t, "first", string(content1))
	assert.Equal(t, "second", string(content2))
}

func TestSave_PathTraversalFilenameStaysInsideStoreDir(t *testing.T) {
	dir := t.TempDir()
	store, err := New(dir)
	require.NoError(t, err)

	path, err := store.Save("../../etc/evil.icd", strings.NewReader("data"))

	require.NoError(t, err)
	rel, err := filepath.Rel(dir, path)
	require.NoError(t, err)
	assert.False(t, strings.HasPrefix(rel, ".."), "saved path %q escaped store dir %q", path, dir)
}

// age backdates a file's mtime so a sweep sees it as old, without the test having to wait.
func age(t *testing.T, path string, by time.Duration) {
	t.Helper()
	when := time.Now().Add(-by)
	require.NoError(t, os.Chtimes(path, when, when))
}

func TestSweep_RemovesAnUploadNobodyEverUsed(t *testing.T) {
	store, err := New(t.TempDir())
	require.NoError(t, err)
	path, err := store.Save("abandoned.icd", strings.NewReader("data"))
	require.NoError(t, err)
	age(t, path, 31*time.Minute)

	removed, err := store.Sweep(nil, MaxUnreferencedAge)

	require.NoError(t, err)
	assert.Equal(t, 1, removed)
	assert.NoFileExists(t, path)
}

func TestSweep_KeepsAFileYoungerThanTheLimit(t *testing.T) {
	store, err := New(t.TempDir())
	require.NoError(t, err)
	path, err := store.Save("fresh.icd", strings.NewReader("data"))
	require.NoError(t, err)

	removed, err := store.Sweep(nil, MaxUnreferencedAge)

	require.NoError(t, err)
	assert.Equal(t, 0, removed)
	assert.FileExists(t, path)
}

// A device that has been running for hours still needs its file: if the daemon crashes, the API
// restarts it and replays every active device's StartParams, sclFilePath included.
func TestSweep_KeepsAnOldFileThatARunningDeviceStillReferences(t *testing.T) {
	store, err := New(t.TempDir())
	require.NoError(t, err)
	inUsePath, err := store.Save("watched.icd", strings.NewReader("data"))
	require.NoError(t, err)
	stalePath, err := store.Save("stale.icd", strings.NewReader("data"))
	require.NoError(t, err)
	age(t, inUsePath, 6*time.Hour)
	age(t, stalePath, 6*time.Hour)

	removed, err := store.Sweep(map[string]bool{inUsePath: true}, MaxUnreferencedAge)

	require.NoError(t, err)
	assert.Equal(t, 1, removed)
	assert.FileExists(t, inUsePath, "a running device's file must survive for crash re-arm")
	assert.NoFileExists(t, stalePath)
}

// How startup clears whatever a kill -9 left behind: nothing can be running yet, so nothing is
// worth keeping regardless of age.
func TestSweep_ZeroAgeWithNoReferencesEmptiesTheDirectory(t *testing.T) {
	dir := t.TempDir()
	store, err := New(dir)
	require.NoError(t, err)
	_, err = store.Save("one.icd", strings.NewReader("data"))
	require.NoError(t, err)
	_, err = store.Save("two.scd", strings.NewReader("data"))
	require.NoError(t, err)

	removed, err := store.Sweep(nil, 0)

	require.NoError(t, err)
	assert.Equal(t, 2, removed)
	entries, err := os.ReadDir(dir)
	require.NoError(t, err)
	assert.Empty(t, entries)
}

func TestTouch_ResetsTheClockOnAFileAboutToBeSwept(t *testing.T) {
	store, err := New(t.TempDir())
	require.NoError(t, err)
	path, err := store.Save("late-connect.icd", strings.NewReader("data"))
	require.NoError(t, err)
	age(t, path, 45*time.Minute)

	store.Touch(path)
	removed, err := store.Sweep(nil, MaxUnreferencedAge)

	require.NoError(t, err)
	assert.Equal(t, 0, removed)
	assert.FileExists(t, path)
}

// sclFilePath may name any file already on the daemon's disk, so neither method may touch or
// delete anything outside the store's own directory.
func TestTouchAndSweep_IgnoreFilesOutsideTheStoreDirectory(t *testing.T) {
	outside := filepath.Join(t.TempDir(), "someones-own.icd")
	require.NoError(t, os.WriteFile(outside, []byte("data"), 0o644))
	age(t, outside, 6*time.Hour)
	before, err := os.Stat(outside)
	require.NoError(t, err)

	store, err := New(t.TempDir())
	require.NoError(t, err)
	store.Touch(outside)
	removed, err := store.Sweep(nil, 0)

	require.NoError(t, err)
	assert.Equal(t, 0, removed)
	assert.FileExists(t, outside)
	after, err := os.Stat(outside)
	require.NoError(t, err)
	assert.Equal(t, before.ModTime(), after.ModTime(), "Touch must not modify a file it doesn't own")
}
