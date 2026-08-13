package streamrelay

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

var testUpgrader = websocket.Upgrader{CheckOrigin: func(r *http.Request) bool { return true }}

// fakeUpstream is a minimal in-process daemon-side stream peer — streamrelay is the boundary
// package, so its unit tests control a real local WS peer rather than mocking anything
// further down.
type fakeUpstream struct {
	srv *httptest.Server

	mu    sync.Mutex
	conns []*websocket.Conn
}

func newFakeUpstream(t *testing.T) *fakeUpstream {
	t.Helper()
	fu := &fakeUpstream{}
	fu.srv = httptest.NewServer(http.HandlerFunc(fu.handle))
	t.Cleanup(fu.srv.Close)
	return fu
}

func (fu *fakeUpstream) handle(w http.ResponseWriter, r *http.Request) {
	conn, err := testUpgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	fu.mu.Lock()
	fu.conns = append(fu.conns, conn)
	fu.mu.Unlock()

	// Keep the connection alive until the client closes it; streamrelay.Hub never sends
	// anything upstream, so there's nothing to read/act on here beyond detecting close.
	for {
		if _, _, err := conn.ReadMessage(); err != nil {
			return
		}
	}
}

func (fu *fakeUpstream) url() string {
	return "ws" + strings.TrimPrefix(fu.srv.URL, "http")
}

// send waits for at least one connection to be accepted, then writes data to all of them.
func (fu *fakeUpstream) send(t *testing.T, data []byte) {
	t.Helper()
	require.Eventually(t, func() bool {
		fu.mu.Lock()
		defer fu.mu.Unlock()
		return len(fu.conns) > 0
	}, 2*time.Second, 10*time.Millisecond)

	fu.mu.Lock()
	defer fu.mu.Unlock()
	for _, c := range fu.conns {
		require.NoError(t, c.WriteMessage(websocket.TextMessage, data))
	}
}

func recvOrTimeout(t *testing.T, ch <-chan []byte) []byte {
	t.Helper()
	select {
	case msg := <-ch:
		return msg
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for message")
		return nil
	}
}

func TestHub_FansOutToMultipleSubscribers(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)
	defer hub.Close()

	ch1, cancel1 := hub.Subscribe()
	defer cancel1()
	ch2, cancel2 := hub.Subscribe()
	defer cancel2()

	fu.send(t, []byte(`{"type":"SCAN_RESULT"}`))

	assert.Equal(t, `{"type":"SCAN_RESULT"}`, string(recvOrTimeout(t, ch1)))
	assert.Equal(t, `{"type":"SCAN_RESULT"}`, string(recvOrTimeout(t, ch2)))
}

func TestHub_SlowSubscriberDropsWithoutBlockingOthers(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)
	defer hub.Close()

	slow, cancelSlow := hub.Subscribe() // never read from
	defer cancelSlow()
	fast, cancelFast := hub.Subscribe()
	defer cancelFast()

	// Send far more messages than the subscriber buffer (DefaultBufferSize) can hold.
	for i := 0; i < DefaultBufferSize*3; i++ {
		fu.send(t, []byte(`{"n":1}`))
	}

	// The fast subscriber must still be able to receive without ever having its channel
	// drained artificially — proves the slow one didn't block broadcast().
	assert.Equal(t, `{"n":1}`, string(recvOrTimeout(t, fast)))

	// The slow subscriber's buffered channel should be full but not deadlocking anything;
	// draining it should yield at most DefaultBufferSize buffered messages.
	drained := 0
	for {
		select {
		case <-slow:
			drained++
		default:
			assert.LessOrEqual(t, drained, DefaultBufferSize)
			return
		}
	}
}

func TestHub_CloseClosesAllSubscriberChannels(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)

	ch, cancelSub := hub.Subscribe()
	defer cancelSub()

	hub.Close()

	_, ok := <-ch
	assert.False(t, ok, "subscriber channel should be closed after Hub.Close()")
}

func TestHub_SubscribeAfterCloseDoesNotPanic(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)
	hub.Close()

	assert.NotPanics(t, func() {
		ch, cancelSub := hub.Subscribe()
		defer cancelSub()
		_ = ch
	})
}

func TestHub_UnsubscribeCancelClosesOnlyThatChannel(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)
	defer hub.Close()

	ch1, cancel1 := hub.Subscribe()
	ch2, cancel2 := hub.Subscribe()
	defer cancel2()

	cancel1()
	_, ok := <-ch1
	assert.False(t, ok)

	fu.send(t, []byte(`{"still":"alive"}`))
	assert.Equal(t, `{"still":"alive"}`, string(recvOrTimeout(t, ch2)))
}

func TestHub_CancelIsIdempotent(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)
	defer hub.Close()

	_, cancelSub := hub.Subscribe()

	assert.NotPanics(t, func() {
		cancelSub()
		cancelSub()
	})
}

func TestHub_ReplaysLastConnectionStatusToNewSubscriber(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)
	defer hub.Close()

	// Witness proves the send was fully processed (and forgotten - no other subscriber left)
	// before the real subscriber under test connects, so that subscriber's receipt can only be
	// explained by the retained-replay path, not a lucky race against ordinary live fan-out —
	// the exact race a real device's fast MMS connect wins against a browser that hasn't
	// opened its websocket yet.
	witness, cancelWitness := hub.Subscribe()
	fu.send(t, []byte(`{"schemaVersion":1,"type":"CONNECTION_STATUS","status":"CONNECTED"}`))
	assert.Equal(t, `{"schemaVersion":1,"type":"CONNECTION_STATUS","status":"CONNECTED"}`,
		string(recvOrTimeout(t, witness)))
	cancelWitness()

	ch, cancelSub := hub.Subscribe()
	defer cancelSub()

	assert.Equal(t, `{"schemaVersion":1,"type":"CONNECTION_STATUS","status":"CONNECTED"}`,
		string(recvOrTimeout(t, ch)))
}

func TestHub_ReplaysOnlyTheMostRecentConnectionStatus(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)
	defer hub.Close()

	// A witness subscriber, held open across both sends, makes each send's processing
	// (including updating Hub.retained) provably complete before we proceed — fu.send only
	// waits for the upstream connection to accept the write, not for the Hub's read loop to
	// have broadcast it yet.
	witness, cancelWitness := hub.Subscribe()

	fu.send(t, []byte(`{"type":"CONNECTION_STATUS","status":"CONNECTED"}`))
	assert.Equal(t, `{"type":"CONNECTION_STATUS","status":"CONNECTED"}`, string(recvOrTimeout(t, witness)))

	fu.send(t, []byte(`{"type":"CONNECTION_STATUS","status":"CONNECTION_REJECTED"}`))
	assert.Equal(t, `{"type":"CONNECTION_STATUS","status":"CONNECTION_REJECTED"}`, string(recvOrTimeout(t, witness)))
	cancelWitness()

	ch, cancelSub := hub.Subscribe()
	defer cancelSub()

	assert.Equal(t, `{"type":"CONNECTION_STATUS","status":"CONNECTION_REJECTED"}`,
		string(recvOrTimeout(t, ch)))
}

func TestHub_DoesNotReplayReportOrScanMessages(t *testing.T) {
	fu := newFakeUpstream(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	hub := NewHub(ctx, fu.url(), nil, DefaultBufferSize)
	defer hub.Close()

	// Witness confirms the send was actually processed (and, since it's the only subscriber
	// at that point, delivered and forgotten) before a later subscriber checks for replay —
	// without this, a subscribe() racing ahead of the Hub's own read-loop processing would
	// legitimately receive the message via normal live fan-out, not replay, and flake the test.
	witness, cancelWitness := hub.Subscribe()
	fu.send(t, []byte(`{"type":"SCAN_RESULT"}`))
	assert.Equal(t, `{"type":"SCAN_RESULT"}`, string(recvOrTimeout(t, witness)))
	cancelWitness()

	ch, cancelSub := hub.Subscribe()
	defer cancelSub()

	select {
	case msg := <-ch:
		t.Fatalf("expected no replay for a non-CONNECTION_STATUS message, got %q", msg)
	case <-time.After(100 * time.Millisecond):
	}
}

func TestHub_DialFailureLogsAndExitsCleanly(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Nothing listens on this port — dialWithRetry should exhaust its retries and give up;
	// Close() must still return promptly rather than hang.
	hub := NewHub(ctx, "ws://127.0.0.1:1", nil, DefaultBufferSize)

	done := make(chan struct{})
	go func() {
		hub.Close()
		close(done)
	}()

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("Close() did not return after a failed dial")
	}
}
