//go:build integration

// Package scanning_test drives the full scanning lifecycle against a real station_signal_daemon
// binary and a real station_signal_api process — nothing mocked. Scanning doesn't need a raw
// GOOSE socket (TCP probe + MMS association only), so this suite needs no root.
package scanning_test

import (
	"fmt"
	"net/http"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/integration_tests/testutil"
)

func TestScanning_FullLifecycle(t *testing.T) {
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

	// Listed while active.
	resp, err = h.Get("/scans")
	require.NoError(t, err)
	require.Equal(t, http.StatusOK, resp.StatusCode)
	var scans []map[string]any
	testutil.DecodeJSON(t, resp, &scans)
	require.Len(t, scans, 1)
	assert.Equal(t, "lo", scans[0]["interfaceId"])
	assert.Equal(t, float64(started.ScanID), scans[0]["scanId"])

	// The scan-result WS endpoint must accept the connection even if nothing has been
	// discovered yet (idle, not rejected) — per the daemon's "silence means nothing changed"
	// semantics relayed verbatim by this API.
	conn := h.DialWS(t, "/ws/scans")
	defer conn.Close()
	_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	_, _, readErr := conn.ReadMessage()
	assert.Error(t, readErr, "expected a read timeout while idle, not an immediate close")

	// Stop and confirm it's gone.
	resp, err = h.Delete(fmt.Sprintf("/scans/%d", started.ScanID))
	require.NoError(t, err)
	require.Equalf(t, http.StatusOK, resp.StatusCode, "body: %s", testutil.BodyString(resp))
	resp.Body.Close()

	resp, err = h.Get("/scans")
	require.NoError(t, err)
	var afterStop []map[string]any
	testutil.DecodeJSON(t, resp, &afterStop)
	assert.Empty(t, afterStop)
}

func TestScanning_StartWithInvalidInterfaceIsRejected(t *testing.T) {
	daemonBin := testutil.BuildDaemon(t)
	h := testutil.StartAPI(t, daemonBin)

	resp, err := h.Post("/scans", map[string]any{"interfaceId": ""})
	require.NoError(t, err)
	defer resp.Body.Close()

	assert.Equal(t, http.StatusBadRequest, resp.StatusCode, testutil.BodyString(resp))
}

func TestScanning_StopUnknownScanReturnsNotFound(t *testing.T) {
	daemonBin := testutil.BuildDaemon(t)
	h := testutil.StartAPI(t, daemonBin)

	resp, err := h.Delete("/scans/999999")
	require.NoError(t, err)
	defer resp.Body.Close()

	assert.Equal(t, http.StatusNotFound, resp.StatusCode, testutil.BodyString(resp))
}
