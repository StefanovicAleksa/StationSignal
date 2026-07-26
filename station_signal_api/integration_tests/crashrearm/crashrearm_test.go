//go:build integration

// Package crashrearm_test kills the real daemon process mid-test and verifies the real API
// process detects the crash, respawns the daemon, reconnects the control channel, and
// re-arms whatever was active — reusing the exact manual sequence already verified by hand
// during development. Uses a scan (not a device) so this suite needs no root.
package crashrearm_test

import (
	"net/http"
	"os/exec"
	"strconv"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/integration_tests/testutil"
)

func TestCrashRearm_RespawnsDaemonAndRearmsActiveScan(t *testing.T) {
	daemonBin := testutil.BuildDaemon(t)
	h := testutil.StartAPI(t, daemonBin)

	resp, err := h.Post("/scans", map[string]any{"interfaceId": "lo", "mmsPort": 102})
	require.NoError(t, err)
	var started struct {
		ScanID int `json:"scanId"`
	}
	testutil.UnmarshalBody(t, resp, &started)
	require.Equal(t, http.StatusCreated, resp.StatusCode)
	require.NotZero(t, started.ScanID)

	daemonPID := testutil.DaemonPID(t, h)
	require.NoError(t, exec.Command("kill", "-9", strconv.Itoa(daemonPID)).Run())

	require.Eventually(t, func() bool {
		resp, err := h.Get("/health")
		if err != nil {
			return false
		}
		defer resp.Body.Close()
		var body struct {
			DaemonRunning           bool `json:"daemonRunning"`
			ControlChannelConnected bool `json:"controlChannelConnected"`
		}
		if err := testutil.DecodeJSONNoFatal(resp, &body); err != nil {
			return false
		}
		return body.DaemonRunning && body.ControlChannelConnected
	}, 15*time.Second, 200*time.Millisecond, "API did not recover (respawn + reconnect) after the daemon was killed")

	// The daemon process itself must actually be a new PID, not the one we just killed.
	newDaemonPID := testutil.DaemonPID(t, h)
	assert.NotEqual(t, daemonPID, newDaemonPID)

	// The scan should have been re-armed automatically — still listed (possibly under a new
	// scanId, since the fresh daemon process assigns its own IDs).
	resp2, err := h.Get("/scans")
	require.NoError(t, err)
	var scans []map[string]any
	testutil.DecodeJSON(t, resp2, &scans)
	require.Len(t, scans, 1, "expected the scan to have been re-armed against the respawned daemon")
	assert.Equal(t, "lo", scans[0]["interfaceId"])
}
