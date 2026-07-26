//go:build integration

// Package testutil provides shared setup for integration tests: building the real daemon
// binary and its manual IED simulator, spawning the real ied_reporter_api against them, and
// small HTTP/WS client helpers. Integration tests mock nothing — everything here drives real
// processes and real sockets. Build-tagged like every integration test file, so `go build
// ./...`/`go vet ./...` never touch it in the default (unit-only) build.
package testutil

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/require"
)

// daemonRepoRelPath is ied_reporter_daemon's location relative to a package under
// ied_reporter_api/integration_tests/<feature>/, i.e. a sibling of ied_reporter_api itself.
const daemonRepoRelPath = "../../../ied_reporter_daemon"

func daemonRepoPath(t *testing.T) string {
	t.Helper()
	path, err := filepath.Abs(daemonRepoRelPath)
	require.NoError(t, err)
	require.DirExistsf(t, path, "expected ied_reporter_daemon checked out as a sibling of ied_reporter_api at %s", path)
	return path
}

// goEnv pins GOTOOLCHAIN=local so nested `go build` invocations never attempt a network
// toolchain download regardless of the invoking shell's environment.
func goEnv() []string {
	return append(os.Environ(), "GOTOOLCHAIN=local")
}

// BuildDaemon builds the real ied_reporter_daemon binary via its own rebuild_proj.sh and
// returns the path to the resulting binary, placed in t.TempDir() so it's cleaned up
// automatically. Fails the test immediately if the build fails.
func BuildDaemon(t *testing.T) string {
	t.Helper()
	repo := daemonRepoPath(t)

	out := filepath.Join(t.TempDir(), "ied_reporter_daemon")
	cmd := exec.Command(filepath.Join(repo, "rebuild_proj.sh"), out)
	cmd.Dir = repo
	output, err := cmd.CombinedOutput()
	require.NoErrorf(t, err, "failed to build ied_reporter_daemon: %s", output)
	require.FileExists(t, out)
	return out
}

// BuildSimulator builds (if not already built) the daemon's own manual IED simulator —
// integration_tests/ied_simulator in the daemon repo — and returns its path. The simulator
// simulates a "Reporter1" IED over real MMS/loopback with no raw-socket/root requirement.
func BuildSimulator(t *testing.T) string {
	t.Helper()
	repo := daemonRepoPath(t)
	simDir := filepath.Join(repo, "integration_tests", "ied_simulator")
	bin := filepath.Join(simDir, "ied_simulator")

	if _, err := os.Stat(bin); err == nil {
		return bin
	}

	cmd := exec.Command("make")
	cmd.Dir = simDir
	output, err := cmd.CombinedOutput()
	require.NoErrorf(t, err, "failed to build ied_simulator: %s", output)
	require.FileExists(t, bin)
	return bin
}

// BuildAPI builds this repo's own ied_reporter_api binary and returns its path.
func BuildAPI(t *testing.T) string {
	t.Helper()
	repoRoot, err := filepath.Abs("../..")
	require.NoError(t, err)

	out := filepath.Join(t.TempDir(), "ied_reporter_api")
	cmd := exec.Command("go", "build", "-o", out, "./cmd/ied_reporter_api")
	cmd.Dir = repoRoot
	cmd.Env = goEnv()
	output, err := cmd.CombinedOutput()
	require.NoErrorf(t, err, "failed to build ied_reporter_api: %s", output)
	require.FileExists(t, out)
	return out
}
