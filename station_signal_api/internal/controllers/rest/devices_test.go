package rest

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/daemonproto"
	reportingdomain "station_signal_api/internal/features/reporting/domain"
)

func TestHandleStartReporting_Success(t *testing.T) {
	reporting := &mockReportingService{startDevice: reportingdomain.Device{ID: 1, WSPort: 9000}}
	mux := Router(newTestAPI(reporting, nil, nil, nil))

	req := httptest.NewRequest(http.MethodPost, "/api/devices", strings.NewReader(`{"host":"10.0.0.5","interfaceId":"eth0"}`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusCreated, rec.Code)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, float64(1), body["deviceId"])
	assert.Equal(t, float64(9000), body["wsPort"])
}

func TestHandleStartReporting_MalformedJSON(t *testing.T) {
	mux := Router(newTestAPI(nil, nil, nil, nil))

	req := httptest.NewRequest(http.MethodPost, "/api/devices", strings.NewReader(`not json`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusBadRequest, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrInvalidArgument)
}

func TestHandleStartReporting_DaemonErrors(t *testing.T) {
	tests := []struct {
		code       string
		wantStatus int
	}{
		{daemonproto.ErrInvalidArgument, http.StatusBadRequest},
		{daemonproto.ErrHostAlreadyRunning, http.StatusConflict},
		{daemonproto.ErrStartInProgress, http.StatusConflict},
		{daemonproto.ErrOrchestrationFailed, http.StatusBadGateway},
		{daemonproto.ErrOutOfMemory, http.StatusServiceUnavailable},
		{daemonproto.ErrPortExhausted, http.StatusServiceUnavailable},
		{daemonproto.ErrDaemonUnreachable, http.StatusServiceUnavailable},
		{"SOME_UNKNOWN_CODE", http.StatusInternalServerError},
	}

	for _, tt := range tests {
		t.Run(tt.code, func(t *testing.T) {
			reporting := &mockReportingService{startErr: &daemonproto.Error{Code: tt.code, Message: "boom"}}
			mux := Router(newTestAPI(reporting, nil, nil, nil))

			req := httptest.NewRequest(http.MethodPost, "/api/devices", strings.NewReader(`{"host":"10.0.0.5","interfaceId":"eth0"}`))
			rec := httptest.NewRecorder()
			mux.ServeHTTP(rec, req)

			assert.Equal(t, tt.wantStatus, rec.Code)
			assertErrorCode(t, rec, tt.code)
		})
	}
}

func TestHandleStartReporting_AuthRequired_ReturnsUnauthorized(t *testing.T) {
	// Pins the exact literal strings station_signal_daemon's own
	// OrchestrationUtils_stageToString/_candidateStatusToString produce for a
	// bootstrap-stage ACSE auth rejection (orchestration_utils.c), so a future
	// daemon wording drift fails this test loudly instead of silently breaking
	// classifyStartReportingError's pattern match.
	stage := "SCL bootstrap"
	detail := "access denied (auth required/rejected)"
	reporting := &mockReportingService{startErr: &daemonproto.Error{
		Code:    daemonproto.ErrOrchestrationFailed,
		Message: "orchestration failed",
		Stage:   &stage,
		Detail:  &detail,
	}}
	mux := Router(newTestAPI(reporting, nil, nil, nil))

	req := httptest.NewRequest(http.MethodPost, "/api/devices", strings.NewReader(`{"host":"10.0.0.5","interfaceId":"eth0"}`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusUnauthorized, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrAuthRequired)
}

func TestHandleStartReporting_OrchestrationFailedOtherwiseUnaffected(t *testing.T) {
	// Same code as the auth-required case, but a stage/detail that doesn't match the
	// access-denied signature - must pass through as plain ORCHESTRATION_FAILED, not be
	// misclassified as AUTH_REQUIRED.
	stage := "SCL bootstrap"
	detail := "no MMS server reachable at any candidate host"
	reporting := &mockReportingService{startErr: &daemonproto.Error{
		Code:    daemonproto.ErrOrchestrationFailed,
		Message: "orchestration failed",
		Stage:   &stage,
		Detail:  &detail,
	}}
	mux := Router(newTestAPI(reporting, nil, nil, nil))

	req := httptest.NewRequest(http.MethodPost, "/api/devices", strings.NewReader(`{"host":"10.0.0.5","interfaceId":"eth0"}`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusBadGateway, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrOrchestrationFailed)
}

func TestHandleStartReporting_NonDaemonErrorIsInternalServerErrorWithoutLeakingDetail(t *testing.T) {
	// A bug on this side of the boundary (not a daemon-reported condition) should never leak
	// its raw message to the client.
	reporting := &mockReportingService{startErr: errors.New("nil pointer dereference in some internal helper")}
	mux := Router(newTestAPI(reporting, nil, nil, nil))

	req := httptest.NewRequest(http.MethodPost, "/api/devices", strings.NewReader(`{"host":"10.0.0.5","interfaceId":"eth0"}`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusInternalServerError, rec.Code)
	assert.NotContains(t, rec.Body.String(), "nil pointer dereference")
}

func TestHandleStopReporting_Success(t *testing.T) {
	reporting := &mockReportingService{}
	mux := Router(newTestAPI(reporting, nil, nil, nil))

	req := httptest.NewRequest(http.MethodDelete, "/api/devices/5", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusOK, rec.Code)
	assert.Equal(t, 5, reporting.gotStopID)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, float64(5), body["deviceId"])
}

func TestHandleStopReporting_InvalidIDParam(t *testing.T) {
	mux := Router(newTestAPI(nil, nil, nil, nil))

	req := httptest.NewRequest(http.MethodDelete, "/api/devices/not-a-number", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusBadRequest, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrInvalidArgument)
}

func TestHandleStopReporting_DeviceNotFound(t *testing.T) {
	reporting := &mockReportingService{stopErr: &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound}}
	mux := Router(newTestAPI(reporting, nil, nil, nil))

	req := httptest.NewRequest(http.MethodDelete, "/api/devices/999", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusNotFound, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrDeviceNotFound)
}

func TestHandleListDevices(t *testing.T) {
	reporting := &mockReportingService{list: []reportingdomain.Device{
		{ID: 1, Host: "10.0.0.5", MMSPort: 102, InterfaceID: "eth0", WSPort: 9000},
	}}
	mux := Router(newTestAPI(reporting, nil, nil, nil))

	req := httptest.NewRequest(http.MethodGet, "/api/devices", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusOK, rec.Code)
	var body []map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	require.Len(t, body, 1)
	assert.Equal(t, "10.0.0.5", body[0]["host"])
	assert.Equal(t, "eth0", body[0]["interfaceId"])
}

func TestHandleListDevices_EmptyReturnsEmptyArrayNotNull(t *testing.T) {
	mux := Router(newTestAPI(&mockReportingService{list: []reportingdomain.Device{}}, nil, nil, nil))

	req := httptest.NewRequest(http.MethodGet, "/api/devices", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusOK, rec.Code)
	assert.JSONEq(t, `[]`, rec.Body.String())
}

func TestClassifyStartReportingError_PassthroughForNonDaemonError(t *testing.T) {
	err := errors.New("boom")
	assert.Same(t, err, classifyStartReportingError(err))
}

func TestClassifyStartReportingError_PassthroughForDifferentCode(t *testing.T) {
	stage := "SCL bootstrap"
	detail := "access denied (auth required/rejected)"
	err := &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound, Stage: &stage, Detail: &detail}
	assert.Same(t, err, classifyStartReportingError(err))
}

func TestClassifyStartReportingError_PassthroughForNilStageOrDetail(t *testing.T) {
	err := &daemonproto.Error{Code: daemonproto.ErrOrchestrationFailed}
	assert.Same(t, err, classifyStartReportingError(err))

	stage := "SCL bootstrap"
	err2 := &daemonproto.Error{Code: daemonproto.ErrOrchestrationFailed, Stage: &stage}
	assert.Same(t, err2, classifyStartReportingError(err2))
}

// assertErrorCode is shared by every controller test file in this package.
func assertErrorCode(t *testing.T, rec *httptest.ResponseRecorder, wantCode string) {
	t.Helper()
	var body struct {
		Error struct {
			Code string `json:"code"`
		} `json:"error"`
	}
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, wantCode, body.Error.Code)
}
