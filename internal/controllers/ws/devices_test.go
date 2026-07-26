package ws

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/gorilla/websocket"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func newTestServer(reporting reportingStreamer, scanning scanningStreamer) (*httptest.Server, string) {
	r := chi.NewRouter()
	RegisterRoutes(r, New(reporting, scanning, nil))
	srv := httptest.NewServer(r)
	return srv, "ws" + strings.TrimPrefix(srv.URL, "http")
}

func TestHandleDeviceStream_UnknownDeviceReturns404(t *testing.T) {
	srv, url := newTestServer(&mockReportingStreamer{ok: false}, &mockScanningStreamer{})
	defer srv.Close()

	//nolint:bodyclose // resp is nil on dial failure in the success case, checked below
	_, resp, err := websocket.DefaultDialer.Dial(url+"/ws/devices/999", nil)

	require.Error(t, err)
	require.NotNil(t, resp)
	assert.Equal(t, http.StatusNotFound, resp.StatusCode)
}

func TestHandleDeviceStream_InvalidIDReturns400(t *testing.T) {
	srv, url := newTestServer(&mockReportingStreamer{}, &mockScanningStreamer{})
	defer srv.Close()

	_, resp, err := websocket.DefaultDialer.Dial(url+"/ws/devices/not-a-number", nil)

	require.Error(t, err)
	require.NotNil(t, resp)
	assert.Equal(t, http.StatusBadRequest, resp.StatusCode)
}

func TestHandleDeviceStream_RelaysMessages(t *testing.T) {
	ch := make(chan []byte, 1)
	streamer := &mockReportingStreamer{ok: true, ch: ch}
	srv, url := newTestServer(streamer, &mockScanningStreamer{})
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/devices/1", nil)
	require.NoError(t, err)
	defer conn.Close()

	ch <- []byte(`{"type":"MMS_REPORT"}`)

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, msg, err := conn.ReadMessage()
	require.NoError(t, err)
	assert.Equal(t, `{"type":"MMS_REPORT"}`, string(msg))
}

func TestHandleDeviceStream_HubClosingClosesConnection(t *testing.T) {
	ch := make(chan []byte)
	streamer := &mockReportingStreamer{ok: true, ch: ch}
	srv, url := newTestServer(streamer, &mockScanningStreamer{})
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/devices/1", nil)
	require.NoError(t, err)
	defer conn.Close()

	close(ch) // simulate the device's hub closing (device stopped / daemon crashed)

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, _, err = conn.ReadMessage()
	assert.Error(t, err, "connection should close once the underlying stream closes")
}

func TestHandleDeviceStream_ClientDisconnectUnsubscribes(t *testing.T) {
	ch := make(chan []byte)
	streamer := &mockReportingStreamer{ok: true, ch: ch}
	srv, url := newTestServer(streamer, &mockScanningStreamer{})
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/devices/1", nil)
	require.NoError(t, err)

	require.NoError(t, conn.Close())

	require.Eventually(t, streamer.cancelWasCalled, 2*time.Second, 10*time.Millisecond)
}
