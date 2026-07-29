package rest

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/daemonproto"
	scanningdomain "station_signal_api/internal/features/scanning/domain"
)

func TestHandleStartScan_Success(t *testing.T) {
	scanning := &mockScanningService{startScan: scanningdomain.Scan{ID: 1}}
	mux := Router(newTestAPI(nil, scanning, nil, nil))

	req := httptest.NewRequest(http.MethodPost, "/api/scans", strings.NewReader(`{"interfaceId":"eth0"}`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusCreated, rec.Code)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, float64(1), body["scanId"])
}

func TestHandleStartScan_MalformedJSON(t *testing.T) {
	mux := Router(newTestAPI(nil, nil, nil, nil))

	req := httptest.NewRequest(http.MethodPost, "/api/scans", strings.NewReader(`not json`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusBadRequest, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrInvalidArgument)
}

func TestHandleStartScan_DaemonErrors(t *testing.T) {
	tests := []struct {
		code       string
		wantStatus int
	}{
		{daemonproto.ErrInvalidArgument, http.StatusBadRequest},
		{daemonproto.ErrDispatcherStartFailed, http.StatusServiceUnavailable},
		{daemonproto.ErrThreadCreateFailed, http.StatusServiceUnavailable},
		{daemonproto.ErrDiscoveryCreateFailed, http.StatusServiceUnavailable},
		{daemonproto.ErrOutOfMemory, http.StatusServiceUnavailable},
	}

	for _, tt := range tests {
		t.Run(tt.code, func(t *testing.T) {
			scanning := &mockScanningService{startErr: &daemonproto.Error{Code: tt.code}}
			mux := Router(newTestAPI(nil, scanning, nil, nil))

			req := httptest.NewRequest(http.MethodPost, "/api/scans", strings.NewReader(`{"interfaceId":"eth0"}`))
			rec := httptest.NewRecorder()
			mux.ServeHTTP(rec, req)

			assert.Equal(t, tt.wantStatus, rec.Code)
			assertErrorCode(t, rec, tt.code)
		})
	}
}

func TestHandleStopScan_Success(t *testing.T) {
	scanning := &mockScanningService{}
	mux := Router(newTestAPI(nil, scanning, nil, nil))

	req := httptest.NewRequest(http.MethodDelete, "/api/scans/2", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusOK, rec.Code)
	assert.Equal(t, 2, scanning.gotStopID)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, float64(2), body["scanId"])
}

func TestHandleStopScan_InvalidIDParam(t *testing.T) {
	mux := Router(newTestAPI(nil, nil, nil, nil))

	req := httptest.NewRequest(http.MethodDelete, "/api/scans/nope", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusBadRequest, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrInvalidArgument)
}

func TestHandleStopScan_ScanNotFound(t *testing.T) {
	scanning := &mockScanningService{stopErr: &daemonproto.Error{Code: daemonproto.ErrScanNotFound}}
	mux := Router(newTestAPI(nil, scanning, nil, nil))

	req := httptest.NewRequest(http.MethodDelete, "/api/scans/999", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusNotFound, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrScanNotFound)
}

func TestHandleListScans(t *testing.T) {
	scanning := &mockScanningService{list: []scanningdomain.Scan{
		{ID: 1, InterfaceID: "eth0", MMSPort: 102, SweepIntervalMs: 0},
	}}
	mux := Router(newTestAPI(nil, scanning, nil, nil))

	req := httptest.NewRequest(http.MethodGet, "/api/scans", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusOK, rec.Code)
	var body []map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	require.Len(t, body, 1)
	assert.Equal(t, "eth0", body[0]["interfaceId"])
}

func TestHandleListScans_EmptyReturnsEmptyArrayNotNull(t *testing.T) {
	mux := Router(newTestAPI(nil, &mockScanningService{list: []scanningdomain.Scan{}}, nil, nil))

	req := httptest.NewRequest(http.MethodGet, "/api/scans", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusOK, rec.Code)
	assert.JSONEq(t, `[]`, rec.Body.String())
}
