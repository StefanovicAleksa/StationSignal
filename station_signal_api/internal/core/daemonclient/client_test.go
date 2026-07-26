package daemonclient

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/core/daemonproto"
)

var testUpgrader = websocket.Upgrader{CheckOrigin: func(r *http.Request) bool { return true }}

// fakeServer is a minimal in-process control-channel peer standing in for the daemon —
// daemonclient is the boundary package, so its unit tests control a real local WS peer
// rather than mocking anything further down.
type fakeServer struct {
	srv     *httptest.Server
	respond func(daemonproto.Request) (daemonproto.Response, bool) // ok=false: don't respond

	mu    sync.Mutex
	conns []*websocket.Conn
}

func newFakeServer(t *testing.T, respond func(daemonproto.Request) (daemonproto.Response, bool)) *fakeServer {
	t.Helper()
	fs := &fakeServer{respond: respond}
	fs.srv = httptest.NewServer(http.HandlerFunc(fs.handle))
	t.Cleanup(fs.srv.Close)
	return fs
}

func (fs *fakeServer) handle(w http.ResponseWriter, r *http.Request) {
	conn, err := testUpgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	fs.mu.Lock()
	fs.conns = append(fs.conns, conn)
	fs.mu.Unlock()

	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			return
		}
		var req daemonproto.Request
		if err := json.Unmarshal(data, &req); err != nil {
			continue
		}
		if fs.respond == nil {
			continue
		}
		resp, ok := fs.respond(req)
		if !ok {
			continue
		}
		out, _ := json.Marshal(resp)
		_ = conn.WriteMessage(websocket.TextMessage, out)
	}
}

func (fs *fakeServer) wsURL() string {
	return "ws" + strings.TrimPrefix(fs.srv.URL, "http")
}

// closeConns forcibly closes every accepted server-side connection, simulating the daemon
// process dying / the connection dropping.
func (fs *fakeServer) closeConns() {
	fs.mu.Lock()
	defer fs.mu.Unlock()
	for _, c := range fs.conns {
		_ = c.Close()
	}
}

// broadcastRaw writes data directly to every accepted connection, bypassing the
// request/response pairing — used to simulate an unsolicited/unmatched response frame.
func (fs *fakeServer) broadcastRaw(t *testing.T, data []byte) {
	t.Helper()
	fs.mu.Lock()
	defer fs.mu.Unlock()
	for _, c := range fs.conns {
		require.NoError(t, c.WriteMessage(websocket.TextMessage, data))
	}
}

func echoSuccess(result string) func(daemonproto.Request) (daemonproto.Response, bool) {
	return func(req daemonproto.Request) (daemonproto.Response, bool) {
		return daemonproto.Response{
			SchemaVersion: 1,
			RequestID:     req.RequestID,
			Success:       true,
			Result:        json.RawMessage(result),
		}, true
	}
}

func waitReconnect(t *testing.T, client *Client) {
	t.Helper()
	select {
	case <-client.Reconnects():
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for client to connect")
	}
}

func startClient(t *testing.T, url string) (*Client, chan struct{}) {
	t.Helper()
	restarts := make(chan struct{}, 1)
	client := New(url, restarts, slog.Default())
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go client.Run(ctx)
	return client, restarts
}

func TestClient_Call_Success(t *testing.T) {
	fs := newFakeServer(t, echoSuccess(`{"scanId":1}`))
	client, restarts := startClient(t, fs.wsURL())
	restarts <- struct{}{}
	waitReconnect(t, client)

	raw, err := client.Call(context.Background(), "START_SCAN", map[string]any{"interfaceId": "eth0"})

	require.NoError(t, err)
	assert.JSONEq(t, `{"scanId":1}`, string(raw))
}

func TestClient_Call_DaemonReportedError(t *testing.T) {
	fs := newFakeServer(t, func(req daemonproto.Request) (daemonproto.Response, bool) {
		return daemonproto.Response{
			RequestID: req.RequestID,
			Success:   false,
			Error:     &daemonproto.Error{Code: daemonproto.ErrDeviceNotFound, Message: "no such device"},
		}, true
	})
	client, restarts := startClient(t, fs.wsURL())
	restarts <- struct{}{}
	waitReconnect(t, client)

	_, err := client.Call(context.Background(), "STOP_REPORTING", map[string]any{"deviceId": 5})

	var derr *daemonproto.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, daemonproto.ErrDeviceNotFound, derr.Code)
}

func TestClient_Call_SuccessFalseWithoutErrorDetail(t *testing.T) {
	fs := newFakeServer(t, func(req daemonproto.Request) (daemonproto.Response, bool) {
		return daemonproto.Response{RequestID: req.RequestID, Success: false}, true
	})
	client, restarts := startClient(t, fs.wsURL())
	restarts <- struct{}{}
	waitReconnect(t, client)

	_, err := client.Call(context.Background(), "START_SCAN", map[string]any{})

	var derr *daemonproto.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, daemonproto.ErrDaemonUnreachable, derr.Code)
}

func TestClient_Call_TimesOutWhenDaemonNeverResponds(t *testing.T) {
	fs := newFakeServer(t, func(daemonproto.Request) (daemonproto.Response, bool) {
		return daemonproto.Response{}, false
	})
	client, restarts := startClient(t, fs.wsURL())
	restarts <- struct{}{}
	waitReconnect(t, client)

	callCtx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	_, err := client.Call(callCtx, "START_SCAN", map[string]any{})

	var derr *daemonproto.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, daemonproto.ErrDaemonUnreachable, derr.Code)
}

func TestClient_Call_NotConnectedYet(t *testing.T) {
	restarts := make(chan struct{}) // never signaled
	client := New("ws://127.0.0.1:1", restarts, slog.Default())

	_, err := client.Call(context.Background(), "START_SCAN", map[string]any{})

	var derr *daemonproto.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, daemonproto.ErrDaemonUnreachable, derr.Code)
}

func TestClient_Call_ConcurrentRequestsDoNotCrossTalk(t *testing.T) {
	fs := newFakeServer(t, func(req daemonproto.Request) (daemonproto.Response, bool) {
		out, _ := json.Marshal(req.RequestID)
		return daemonproto.Response{RequestID: req.RequestID, Success: true, Result: out}, true
	})
	client, restarts := startClient(t, fs.wsURL())
	restarts <- struct{}{}
	waitReconnect(t, client)

	const n = 20
	var wg sync.WaitGroup
	errs := make([]error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, err := client.Call(context.Background(), "START_SCAN", map[string]any{"i": i})
			errs[i] = err
		}(i)
	}
	wg.Wait()

	for i, err := range errs {
		assert.NoErrorf(t, err, "call %d failed", i)
	}
}

func TestClient_Run_ReconnectsAfterDisconnect(t *testing.T) {
	fs := newFakeServer(t, echoSuccess(`{}`))
	client, restarts := startClient(t, fs.wsURL())

	restarts <- struct{}{}
	waitReconnect(t, client)
	assert.True(t, client.Connected())

	fs.closeConns()
	require.Eventually(t, func() bool { return !client.Connected() }, 2*time.Second, 10*time.Millisecond)

	restarts <- struct{}{}
	waitReconnect(t, client)
	assert.True(t, client.Connected())
}

func TestClient_readLoop_DropsResponseWithUnknownRequestID(t *testing.T) {
	fs := newFakeServer(t, echoSuccess(`{"ok":true}`))
	client, restarts := startClient(t, fs.wsURL())
	restarts <- struct{}{}
	waitReconnect(t, client)

	bogus, err := json.Marshal(daemonproto.Response{RequestID: "does-not-exist", Success: true, Result: json.RawMessage(`{}`)})
	require.NoError(t, err)
	fs.broadcastRaw(t, bogus)

	raw, err := client.Call(context.Background(), "START_SCAN", map[string]any{})
	require.NoError(t, err)
	assert.JSONEq(t, `{"ok":true}`, string(raw))
}
