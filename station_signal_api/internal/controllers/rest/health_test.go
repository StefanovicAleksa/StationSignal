package rest

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestHandleHealth_ReportsUnderlyingState(t *testing.T) {
	tests := []struct {
		name      string
		running   bool
		connected bool
	}{
		{name: "both up", running: true, connected: true},
		{name: "process up, control channel not yet connected", running: true, connected: false},
		{name: "both down", running: false, connected: false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			mux := Router(newTestAPI(nil, nil, &mockDaemonSupervisor{running: tt.running}, &mockDaemonStatus{connected: tt.connected}))

			req := httptest.NewRequest(http.MethodGet, "/api/health", nil)
			rec := httptest.NewRecorder()
			mux.ServeHTTP(rec, req)

			require.Equal(t, http.StatusOK, rec.Code)
			var body map[string]any
			require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
			assert.Equal(t, tt.running, body["daemonRunning"])
			assert.Equal(t, tt.connected, body["controlChannelConnected"])
		})
	}
}
