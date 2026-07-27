//go:build integration

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

// TestScanning_SessionIsolation drives two independent "browsers" (separate cookie jars, same
// running API/daemon) and confirms one session's scan is invisible to, and unstoppable by, the
// other — the whole point of internal/core/session's per-browser identity.
func TestScanning_SessionIsolation(t *testing.T) {
	daemonBin := testutil.BuildDaemon(t)
	h := testutil.StartAPI(t, daemonBin)
	other := h.NewSession(t)

	resp, err := h.Post("/scans", map[string]any{"interfaceId": "lo", "mmsPort": 102})
	require.NoError(t, err)
	var started struct {
		ScanID int `json:"scanId"`
	}
	testutil.UnmarshalBody(t, resp, &started)
	require.Equal(t, http.StatusCreated, resp.StatusCode)
	require.NotZero(t, started.ScanID)

	// The originating session sees its own scan.
	resp, err = h.Get("/scans")
	require.NoError(t, err)
	var ownScans []map[string]any
	testutil.DecodeJSON(t, resp, &ownScans)
	require.Len(t, ownScans, 1)
	assert.Equal(t, float64(started.ScanID), ownScans[0]["scanId"])

	// A different session's GET /scans must not see it at all.
	resp, err = other.Get("/scans")
	require.NoError(t, err)
	var otherScans []map[string]any
	testutil.DecodeJSON(t, resp, &otherScans)
	assert.Empty(t, otherScans, "a different session must not see another session's scans")

	// A different session's /ws/scans stream must not relay results from a scan it doesn't own.
	conn := other.DialWS(t, "/ws/scans")
	defer conn.Close()
	_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	_, _, readErr := conn.ReadMessage()
	assert.Error(t, readErr, "a different session's scan stream should stay idle, not relay another session's results")

	// A different session cannot stop it either — rejected identically to "doesn't exist".
	resp, err = other.Delete(fmt.Sprintf("/scans/%d", started.ScanID))
	require.NoError(t, err)
	assert.Equalf(t, http.StatusNotFound, resp.StatusCode, "body: %s", testutil.BodyString(resp))

	// The originating session's scan is untouched by the other session's failed attempt.
	resp, err = h.Get("/scans")
	require.NoError(t, err)
	var stillOwnScans []map[string]any
	testutil.DecodeJSON(t, resp, &stillOwnScans)
	assert.Len(t, stillOwnScans, 1, "the owning session's scan must survive another session's rejected stop attempt")

	// The originating session can still stop its own scan.
	resp, err = h.Delete(fmt.Sprintf("/scans/%d", started.ScanID))
	require.NoError(t, err)
	assert.Equalf(t, http.StatusOK, resp.StatusCode, "body: %s", testutil.BodyString(resp))
}
