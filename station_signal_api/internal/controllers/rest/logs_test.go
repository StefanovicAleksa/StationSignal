package rest

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/logfiles"
)

func newLogsTestAPI(store *mockLogFileStore, devMode bool) *API {
	if store == nil {
		store = &mockLogFileStore{}
	}
	return New(&mockReportingService{}, &mockScanningService{}, &mockDaemonSupervisor{},
		&mockDaemonStatus{}, &mockStructureFileStore{}, &mockNetworkService{}, store, devMode, nil)
}

func postClearLogs(t *testing.T, api *API) *httptest.ResponseRecorder {
	t.Helper()
	mux := Router(api)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/api/settings/logs/clear", nil))
	return rec
}

func TestClearLogs_ClearsAndReportsWhatItFreed(t *testing.T) {
	store := &mockLogFileStore{result: logfiles.Result{
		Dir:          "/var/log/station_signal",
		ClearedCount: 12,
		SkippedCount: 0,
		BytesFreed:   14680064,
	}}

	rec := postClearLogs(t, newLogsTestAPI(store, true))

	require.Equal(t, http.StatusOK, rec.Code)
	assert.Equal(t, 1, store.clearCalls)

	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, "/var/log/station_signal", body["logDir"])
	assert.Equal(t, float64(12), body["clearedCount"])
	assert.Equal(t, float64(0), body["skippedCount"])
	assert.Equal(t, float64(14680064), body["bytesFreed"])
}

// The route must not exist at all in prod, rather than existing and refusing. Driven through
// Router (not the handler directly) because the registration itself is the thing under test —
// calling handleClearLogs directly would pass no matter how the gate was written.
func TestClearLogs_RouteIsNotRegisteredInProdMode(t *testing.T) {
	store := &mockLogFileStore{}

	rec := postClearLogs(t, newLogsTestAPI(store, false))

	assert.Equal(t, http.StatusNotFound, rec.Code)
	assert.Zero(t, store.clearCalls, "a prod box must never reach the log store at all")
}

// Turning logging up on a prod box must not hand out a destructive endpoint. This is the one way
// the gate could plausibly be rewritten wrong — the access-log middleware a few lines above it in
// Router() keys off exactly that signal, so copying its idiom here would be an easy mistake.
func TestClearLogs_DebugLoggingDoesNotUnlockTheRouteInProdMode(t *testing.T) {
	store := &mockLogFileStore{}
	api := newLogsTestAPI(store, false)
	api.logger = debugLogger()

	rec := postClearLogs(t, api)

	assert.Equal(t, http.StatusNotFound, rec.Code)
	assert.Zero(t, store.clearCalls)
}

// A partial clear is still a success: one root-owned file that could not be truncated must not
// cost the caller the eleven that were. The count is reported so the UI can say so.
func TestClearLogs_PartialClearIsStillOK(t *testing.T) {
	store := &mockLogFileStore{result: logfiles.Result{
		Dir:          "/var/log/station_signal",
		ClearedCount: 11,
		SkippedCount: 1,
		BytesFreed:   512,
	}}

	rec := postClearLogs(t, newLogsTestAPI(store, true))

	require.Equal(t, http.StatusOK, rec.Code)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, float64(11), body["clearedCount"])
	assert.Equal(t, float64(1), body["skippedCount"])
}

func TestClearLogs_StoreFailureIsAnInternalError(t *testing.T) {
	store := &mockLogFileStore{err: errors.New("read-only filesystem")}

	rec := postClearLogs(t, newLogsTestAPI(store, true))

	assert.Equal(t, http.StatusInternalServerError, rec.Code)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	errObj, ok := body["error"].(map[string]any)
	require.True(t, ok)
	assert.Equal(t, "INTERNAL", errObj["code"])
}

// GET must not work — mutating routes here are POST-only so a plain link or a stray cross-origin
// navigation can't wipe a box's logs.
func TestClearLogs_IsNotReachableByGet(t *testing.T) {
	mux := Router(newLogsTestAPI(nil, true))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/settings/logs/clear", nil))

	assert.Equal(t, http.StatusMethodNotAllowed, rec.Code)
}
