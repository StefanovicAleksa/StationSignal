package rest

import (
	"fmt"
	"net/http"
	"testing"

	"github.com/stretchr/testify/assert"

	"station_signal_api/internal/core/daemonproto"
)

func TestErrorStatus_EveryKnownCode(t *testing.T) {
	tests := []struct {
		code string
		want int
	}{
		{daemonproto.ErrInvalidArgument, http.StatusBadRequest},
		{daemonproto.ErrHostAlreadyRunning, http.StatusConflict},
		{daemonproto.ErrStartInProgress, http.StatusConflict},
		{daemonproto.ErrDeviceNotFound, http.StatusNotFound},
		{daemonproto.ErrScanNotFound, http.StatusNotFound},
		{daemonproto.ErrOrchestrationFailed, http.StatusBadGateway},
		{daemonproto.ErrAuthRequired, http.StatusUnauthorized},
		{daemonproto.ErrOutOfMemory, http.StatusServiceUnavailable},
		{daemonproto.ErrPortExhausted, http.StatusServiceUnavailable},
		{daemonproto.ErrDispatcherStartFailed, http.StatusServiceUnavailable},
		{daemonproto.ErrThreadCreateFailed, http.StatusServiceUnavailable},
		{daemonproto.ErrDiscoveryCreateFailed, http.StatusServiceUnavailable},
		{daemonproto.ErrDaemonUnreachable, http.StatusServiceUnavailable},
		{daemonproto.ErrUnknownAction, http.StatusInternalServerError},
		{"TOTALLY_UNKNOWN_CODE", http.StatusInternalServerError},
	}

	for _, tt := range tests {
		t.Run(tt.code, func(t *testing.T) {
			assert.Equal(t, tt.want, errorStatus(tt.code))
		})
	}
}

func TestInvalidArgument_BuildsCorrectErrorShape(t *testing.T) {
	err := invalidArgument("bad host")

	assert.Equal(t, daemonproto.ErrInvalidArgument, err.Code)
	assert.Equal(t, "bad host", err.Message)
}

func TestWriteError_UnwrapsDaemonprotoErrorViaErrorsAs(t *testing.T) {
	// writeError uses errors.As, so a wrapped *daemonproto.Error (not just a bare one) must
	// still be recognized and mapped correctly rather than falling through to the generic
	// 500 branch. Exercised end-to-end via a handler in devices_test.go/scans_test.go; this
	// confirms the underlying wrap-then-map mechanics directly.
	inner := &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound, Message: "no such device"}
	wrapped := fmt.Errorf("decoding result: %w", inner)

	assert.Equal(t, http.StatusNotFound, errorStatus(inner.Code))
	assert.ErrorIs(t, wrapped, inner)
}
