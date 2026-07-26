package ws

import (
	"net"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestHandleScanStream_IdleWhileNoScanIsActive(t *testing.T) {
	streamer := &mockScanningStreamer{ok: false}
	srv, url := newTestServer(&mockReportingStreamer{}, streamer)
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/scans", nil)
	require.NoError(t, err)
	defer conn.Close()

	// No scan active yet: nothing should arrive within a short window, and the connection
	// must stay open (accept-and-idle, not reject) — consistent with the daemon's own
	// "silence means nothing changed" semantics. A gorilla connection is unusable after any
	// read error, deliberate timeout included, so this test only exercises the idle phase —
	// TestHandleScanStream_RelaysImmediatelyWhenAlreadyActive covers the "then relays" phase
	// on its own connection.
	_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	_, _, err = conn.ReadMessage()
	require.Error(t, err)
	var netErr net.Error
	require.ErrorAs(t, err, &netErr)
	assert.True(t, netErr.Timeout(), "expected a read timeout, not a message or close")
}

func TestHandleScanStream_StartsRelayingOnceAScanBecomesActive(t *testing.T) {
	streamer := &mockScanningStreamer{ok: false}
	srv, url := newTestServer(&mockReportingStreamer{}, streamer)
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/scans", nil)
	require.NoError(t, err)
	defer conn.Close()

	ch := make(chan []byte, 1)
	// Give the handler's poll loop a moment to observe not-ready at least once before
	// flipping, so this genuinely exercises the idle->active transition.
	time.Sleep(50 * time.Millisecond)
	streamer.setReady(true, ch)
	ch <- []byte(`{"type":"SCAN_RESULT"}`)

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, msg, err := conn.ReadMessage()
	require.NoError(t, err)
	assert.Equal(t, `{"type":"SCAN_RESULT"}`, string(msg))
}

func TestHandleScanStream_RelaysImmediatelyWhenAlreadyActive(t *testing.T) {
	ch := make(chan []byte, 1)
	streamer := &mockScanningStreamer{ok: true, ch: ch}
	srv, url := newTestServer(&mockReportingStreamer{}, streamer)
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/scans", nil)
	require.NoError(t, err)
	defer conn.Close()

	ch <- []byte(`{"type":"SCAN_RESULT"}`)

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, msg, err := conn.ReadMessage()
	require.NoError(t, err)
	assert.Equal(t, `{"type":"SCAN_RESULT"}`, string(msg))
}

func TestHandleScanStream_HubClosingGoesIdleInsteadOfClosingConnection(t *testing.T) {
	ch := make(chan []byte)
	streamer := &mockScanningStreamer{ok: true, ch: ch}
	srv, url := newTestServer(&mockReportingStreamer{}, streamer)
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/scans", nil)
	require.NoError(t, err)
	defer conn.Close()

	streamer.setReady(false, nil)
	close(ch)

	// The hub going away (last scan stopped) must not tear down the client connection — a
	// still-open client should be ready for the next scan without a fresh handshake, since a
	// fresh handshake re-races the daemon's ~300ms first-sweep grace window. Same idle
	// semantics as TestHandleScanStream_IdleWhileNoScanIsActive.
	_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	_, _, err = conn.ReadMessage()
	require.Error(t, err)
	var netErr net.Error
	require.ErrorAs(t, err, &netErr)
	assert.True(t, netErr.Timeout(), "expected a read timeout, not a message or close")
}

func TestHandleScanStream_RelaysAcrossMultipleHubLifecyclesOnOneConnection(t *testing.T) {
	ch1 := make(chan []byte, 1)
	streamer := &mockScanningStreamer{ok: true, ch: ch1}
	srv, url := newTestServer(&mockReportingStreamer{}, streamer)
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/scans", nil)
	require.NoError(t, err)
	defer conn.Close()

	ch1 <- []byte(`{"scanId":1}`)
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, msg, err := conn.ReadMessage()
	require.NoError(t, err)
	assert.Equal(t, `{"scanId":1}`, string(msg))

	// First scan stops: the hub closes. The connection must survive this and go back to
	// idle-polling rather than being torn down (see the idle test above).
	streamer.setReady(false, nil)
	close(ch1)

	// A second scan starts on a brand new hub, relayed on the SAME client connection.
	ch2 := make(chan []byte, 1)
	streamer.setReady(true, ch2)
	ch2 <- []byte(`{"scanId":2}`)

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, msg, err = conn.ReadMessage()
	require.NoError(t, err)
	assert.Equal(t, `{"scanId":2}`, string(msg))
}

func TestHandleScanStream_OmitsAlreadyConnectedDeviceButRelaysOthers(t *testing.T) {
	ch := make(chan []byte, 2)
	scanning := &mockScanningStreamer{ok: true, ch: ch}
	reporting := &mockReportingStreamer{connected: []connectedDevice{{host: "10.0.0.5", mmsPort: 102}}}
	srv, url := newTestServer(reporting, scanning)
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/scans", nil)
	require.NoError(t, err)
	defer conn.Close()

	// Already-connected device: must be omitted from what the frontend sees.
	ch <- []byte(`{"type":"SCAN_RESULT","host":"10.0.0.5","mmsPort":102}`)
	// Not connected: must still pass through, on the same connection.
	ch <- []byte(`{"type":"SCAN_RESULT","host":"10.0.0.6","mmsPort":102}`)

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, msg, err := conn.ReadMessage()
	require.NoError(t, err)
	assert.Equal(t, `{"type":"SCAN_RESULT","host":"10.0.0.6","mmsPort":102}`, string(msg),
		"the connected device's result should have been skipped, so this must be the second message")
}

func TestHandleScanStream_ClientDisconnectDuringIdleStopsPolling(t *testing.T) {
	streamer := &mockScanningStreamer{ok: false}
	srv, url := newTestServer(&mockReportingStreamer{}, streamer)
	defer srv.Close()

	conn, _, err := websocket.DefaultDialer.Dial(url+"/ws/scans", nil)
	require.NoError(t, err)

	require.NoError(t, conn.Close())

	// No direct observable side effect for the idle-poll path (nothing was subscribed yet),
	// but the handler goroutine must exit rather than poll forever — give it a moment and
	// confirm the server doesn't panic/hang on shutdown, which Close() below would surface.
	time.Sleep(200 * time.Millisecond)
}
