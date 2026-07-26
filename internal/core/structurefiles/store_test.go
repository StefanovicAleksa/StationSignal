package structurefiles

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

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
