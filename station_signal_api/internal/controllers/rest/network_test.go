package rest

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	networkdomain "station_signal_api/internal/features/network/domain"
)

func newNetworkTestAPI(network *mockNetworkService) *API {
	if network == nil {
		network = &mockNetworkService{}
	}
	return New(&mockReportingService{}, &mockScanningService{}, &mockDaemonSupervisor{}, &mockDaemonStatus{}, &mockStructureFileStore{}, network, &mockLogFileStore{}, true, nil)
}

func TestHandleGetNetworkStatus_Success(t *testing.T) {
	network := &mockNetworkService{status: networkdomain.Status{
		Interface:       "eth0",
		Current:         networkdomain.Config{CIDR: "192.168.1.50/24"},
		RecoveryAddress: networkdomain.RecoveryAddress,
	}}
	mux := Router(newNetworkTestAPI(network))

	req := httptest.NewRequest(http.MethodGet, "/api/settings/network", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusOK, rec.Code)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, "eth0", body["interface"])
	assert.Equal(t, "169.254.1.1", body["recoveryAddress"])
}

func TestHandleGetNetworkStatus_Failure(t *testing.T) {
	network := &mockNetworkService{statusErr: &networkdomain.Error{Code: networkdomain.ErrApplyFailed, Message: "ip: not found"}}
	mux := Router(newNetworkTestAPI(network))

	req := httptest.NewRequest(http.MethodGet, "/api/settings/network", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusInternalServerError, rec.Code)
}

func TestHandleApplyNetworkConfig_Success(t *testing.T) {
	expiresAt := time.Now().Add(90 * time.Second)
	network := &mockNetworkService{pending: networkdomain.PendingChange{
		New:       networkdomain.Config{CIDR: "192.168.1.60/24"},
		ExpiresAt: expiresAt,
	}}
	mux := Router(newNetworkTestAPI(network))

	req := httptest.NewRequest(http.MethodPost, "/api/settings/network", strings.NewReader(`{"cidr":"192.168.1.60/24"}`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusAccepted, rec.Code)
	assert.Equal(t, "192.168.1.60/24", network.gotApplyConfig.CIDR)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	newCfg, ok := body["new"].(map[string]any)
	require.True(t, ok)
	assert.Equal(t, "192.168.1.60/24", newCfg["cidr"])
}

func TestHandleApplyNetworkConfig_MalformedJSON(t *testing.T) {
	mux := Router(newNetworkTestAPI(nil))

	req := httptest.NewRequest(http.MethodPost, "/api/settings/network", strings.NewReader(`not json`))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusBadRequest, rec.Code)
	assertErrorCode(t, rec, networkdomain.ErrInvalidArgument)
}

func TestHandleApplyNetworkConfig_ServiceErrors(t *testing.T) {
	tests := []struct {
		code       string
		wantStatus int
	}{
		{networkdomain.ErrInvalidArgument, http.StatusBadRequest},
		{networkdomain.ErrSessionsActive, http.StatusConflict},
		{networkdomain.ErrChangeAlreadyPending, http.StatusConflict},
		{networkdomain.ErrApplyFailed, http.StatusInternalServerError},
	}

	for _, tt := range tests {
		t.Run(tt.code, func(t *testing.T) {
			network := &mockNetworkService{applyErr: &networkdomain.Error{Code: tt.code, Message: "boom"}}
			mux := Router(newNetworkTestAPI(network))

			req := httptest.NewRequest(http.MethodPost, "/api/settings/network", strings.NewReader(`{"cidr":"192.168.1.60/24"}`))
			rec := httptest.NewRecorder()
			mux.ServeHTTP(rec, req)

			assert.Equal(t, tt.wantStatus, rec.Code)
			assertErrorCode(t, rec, tt.code)
		})
	}
}

func TestHandleApplyNetworkConfig_IsNotReachableViaGET(t *testing.T) {
	mux := Router(newNetworkTestAPI(nil))

	req := httptest.NewRequest(http.MethodGet, "/api/settings/network/confirm", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.NotEqual(t, http.StatusOK, rec.Code, "the confirm endpoint must not be triggerable by a GET request")
}

func TestHandleConfirmNetworkConfig_Success(t *testing.T) {
	network := &mockNetworkService{}
	mux := Router(newNetworkTestAPI(network))

	req := httptest.NewRequest(http.MethodPost, "/api/settings/network/confirm", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusOK, rec.Code)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, true, body["confirmed"])
}

func TestHandleConfirmNetworkConfig_NoPendingChange(t *testing.T) {
	network := &mockNetworkService{confirmErr: &networkdomain.Error{Code: networkdomain.ErrNoPendingChange, Message: "nothing pending"}}
	mux := Router(newNetworkTestAPI(network))

	req := httptest.NewRequest(http.MethodPost, "/api/settings/network/confirm", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusConflict, rec.Code)
	assertErrorCode(t, rec, networkdomain.ErrNoPendingChange)
}

func TestHandleRevertNetworkConfig_Success(t *testing.T) {
	network := &mockNetworkService{}
	mux := Router(newNetworkTestAPI(network))

	req := httptest.NewRequest(http.MethodPost, "/api/settings/network/revert", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusOK, rec.Code)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, true, body["reverted"])
	assert.Equal(t, 1, network.revertCalls)
}

func TestHandleRevertNetworkConfig_NoPendingChange(t *testing.T) {
	network := &mockNetworkService{revertErr: &networkdomain.Error{Code: networkdomain.ErrNoPendingChange, Message: "nothing pending"}}
	mux := Router(newNetworkTestAPI(network))

	req := httptest.NewRequest(http.MethodPost, "/api/settings/network/revert", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusConflict, rec.Code)
	assertErrorCode(t, rec, networkdomain.ErrNoPendingChange)
}

// A revert that cleared the wedge but couldn't restore the old address still has to reach the
// technician as a failure — the box may not be back where they expect it.
func TestHandleRevertNetworkConfig_PartialFailureIsReported(t *testing.T) {
	network := &mockNetworkService{revertErr: &networkdomain.Error{Code: networkdomain.ErrApplyFailed, Message: "reverted, but nmcli could not reactivate"}}
	mux := Router(newNetworkTestAPI(network))

	req := httptest.NewRequest(http.MethodPost, "/api/settings/network/revert", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusInternalServerError, rec.Code)
	assertErrorCode(t, rec, networkdomain.ErrApplyFailed)
}
