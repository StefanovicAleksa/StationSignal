package logfiles

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func writeFile(t *testing.T, dir, name, content string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	require.NoError(t, os.WriteFile(path, []byte(content), 0o644))
	return path
}

func sizeOf(t *testing.T, path string) int64 {
	t.Helper()
	info, err := os.Stat(path)
	require.NoError(t, err)
	return info.Size()
}

func TestClear_EmptiesEveryLogFileAndReportsWhatItFreed(t *testing.T) {
	dir := t.TempDir()
	daemonLog := writeFile(t, dir, "station-signal-mms_report_client.log", "0123456789")
	apiLog := writeFile(t, dir, "station-signal-api.log", "abcde")

	result, err := New(dir).Clear()

	require.NoError(t, err)
	assert.Equal(t, 2, result.ClearedCount)
	assert.Equal(t, 0, result.SkippedCount)
	assert.Equal(t, int64(15), result.BytesFreed, "BytesFreed must be the size before truncation")
	assert.Equal(t, dir, result.Dir)
	assert.Zero(t, sizeOf(t, daemonLog))
	assert.Zero(t, sizeOf(t, apiLog))
}

// The files must still EXIST afterwards, not just be empty. This is the whole reason the package
// truncates instead of deleting: the daemon holds an open append-mode handle on each of these for
// its entire process lifetime and never reopens, so unlinking one silently ends logging for that
// feature until the daemon restarts.
func TestClear_TruncatesInPlaceRatherThanDeleting(t *testing.T) {
	dir := t.TempDir()
	path := writeFile(t, dir, "station-signal-daemon.log", "some earlier session")

	// A live writer, holding the file open in append mode exactly as log.h does.
	writer, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND, 0o644)
	require.NoError(t, err)
	defer writer.Close()

	_, err = New(dir).Clear()
	require.NoError(t, err)

	// The handle opened before the clear must still reach the same file, and its next write must
	// land at offset 0 — no orphaned inode, and no run of NUL padding standing in for the old
	// contents.
	_, err = writer.WriteString("after\n")
	require.NoError(t, err)

	contents, err := os.ReadFile(path)
	require.NoError(t, err)
	assert.Equal(t, "after\n", string(contents))
}

func TestClear_LeavesNonLogFilesAlone(t *testing.T) {
	dir := t.TempDir()
	unrelated := writeFile(t, dir, "notes.txt", "keep me")
	otherPrefix := writeFile(t, dir, "some-other-service.log", "keep me too")

	result, err := New(dir).Clear()

	require.NoError(t, err)
	assert.Equal(t, 0, result.ClearedCount)
	assert.Equal(t, int64(7), sizeOf(t, unrelated))
	assert.Equal(t, int64(11), sizeOf(t, otherPrefix))
}

// A log in a SUBdirectory matches neither the glob nor owns(); asserting it explicitly keeps the
// containment guard honest if the glob is ever loosened to `**`.
func TestClear_DoesNotDescendIntoSubdirectories(t *testing.T) {
	dir := t.TempDir()
	nested := filepath.Join(dir, "archive")
	require.NoError(t, os.MkdirAll(nested, 0o755))
	buried := writeFile(t, nested, "station-signal-daemon.log", "archived run")

	result, err := New(dir).Clear()

	require.NoError(t, err)
	assert.Equal(t, 0, result.ClearedCount)
	assert.Equal(t, int64(12), sizeOf(t, buried))
}

func TestClear_MissingDirectoryIsNotAnError(t *testing.T) {
	result, err := New(filepath.Join(t.TempDir(), "never-created")).Clear()

	require.NoError(t, err, "nothing logged yet is a normal state, not a failure")
	assert.Equal(t, 0, result.ClearedCount)
	assert.Equal(t, 0, result.SkippedCount)
}

// The partial-success contract: one unwritable file must not cost the caller every other log.
// Skipped as root, where the permission bits would not actually stop the truncate.
func TestClear_UnwritableFileIsSkippedWhileTheRestAreCleared(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root: file permissions do not deny truncation")
	}

	dir := t.TempDir()
	clearable := writeFile(t, dir, "station-signal-daemon.log", "0123456789")
	locked := writeFile(t, dir, "station-signal-api.log", "root owned in production")
	require.NoError(t, os.Chmod(locked, 0o444))

	result, err := New(dir).Clear()

	require.NoError(t, err, "a permission-denied log is reported via SkippedCount, not as an error")
	assert.Equal(t, 1, result.ClearedCount)
	assert.Equal(t, 1, result.SkippedCount)
	assert.Equal(t, int64(10), result.BytesFreed)
	assert.Zero(t, sizeOf(t, clearable))
	assert.NotZero(t, sizeOf(t, locked))
}

// The privileged fallback is only reachable for the one directory the helper script hardcodes, so
// a store pointed anywhere else must never shell out to sudo — including in tests, which is what
// makes this assertion safe to run.
func TestClear_DoesNotInvokeHelperForANonStandardDirectory(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root: file permissions do not deny truncation")
	}

	dir := t.TempDir()
	locked := writeFile(t, dir, "station-signal-api.log", "root owned in production")
	require.NoError(t, os.Chmod(locked, 0o444))

	store := New(dir).WithPrivilegedHelper("/nonexistent/helper-that-must-not-run.sh")
	result, err := store.Clear()

	require.NoError(t, err, "the helper must not have run, so there is no helper error to report")
	assert.Equal(t, 1, result.SkippedCount)
}

func TestWithPrivilegedHelper_DoesNotMutateTheOriginal(t *testing.T) {
	base := New(t.TempDir())
	withHelper := base.WithPrivilegedHelper("/opt/station_signal/bin/station-signal-clearlogs.sh")

	assert.Empty(t, base.helperPath)
	assert.NotEmpty(t, withHelper.helperPath)
}
