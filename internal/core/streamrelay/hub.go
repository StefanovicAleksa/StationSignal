// Package streamrelay fans a single daemon-side push-only websocket stream (a per-device
// report stream, or the shared scan-result stream) out to any number of frontend
// subscribers. The frontend never dials the daemon's ports directly — wsapi subscribes to a
// Hub instead.
package streamrelay

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const (
	subscriberBufferSize = 16
	dialRetries          = 5
	dialRetryDelay       = 20 * time.Millisecond
)

// Hub dials one daemon-side stream URL and rebroadcasts every frame it reads, verbatim, to
// each current subscriber. A subscriber that can't keep up has its backlog dropped rather
// than queued — the same "no replay" semantics the daemon itself applies to its own stream
// consumers.
type Hub struct {
	url    string
	logger *slog.Logger

	mu   sync.Mutex
	subs map[chan []byte]struct{}

	cancel context.CancelFunc
	done   chan struct{}
}

// NewHub dials url and starts relaying immediately. Call Close when the underlying daemon
// resource (device or scan) goes away.
func NewHub(ctx context.Context, url string, logger *slog.Logger) *Hub {
	if logger == nil {
		logger = slog.Default()
	}
	hctx, cancel := context.WithCancel(ctx)
	h := &Hub{
		url:    url,
		logger: logger,
		subs:   make(map[chan []byte]struct{}),
		cancel: cancel,
		done:   make(chan struct{}),
	}
	go h.run(hctx)
	return h
}

// Subscribe registers a new frontend subscriber and returns its message channel plus a
// cancel function the caller must invoke exactly once when done (e.g. on client
// disconnect). The channel is closed once cancel is called or the Hub itself closes.
func (h *Hub) Subscribe() (<-chan []byte, func()) {
	ch := make(chan []byte, subscriberBufferSize)
	h.mu.Lock()
	h.subs[ch] = struct{}{}
	h.mu.Unlock()

	var once sync.Once
	cancel := func() {
		once.Do(func() {
			h.mu.Lock()
			if _, ok := h.subs[ch]; ok {
				delete(h.subs, ch)
				close(ch)
			}
			h.mu.Unlock()
		})
	}
	return ch, cancel
}

// Close tears down the upstream connection and closes every subscriber channel.
func (h *Hub) Close() {
	h.cancel()
	<-h.done

	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.subs {
		close(ch)
	}
	// Reset (not nil) so a Subscribe() racing just after Close() can't panic on assignment
	// to a nil map — it'll register into a hub that's already stopped relaying, which is
	// harmless (the subscriber simply never receives anything until its own caller gives up).
	h.subs = make(map[chan []byte]struct{})
}

func (h *Hub) run(ctx context.Context) {
	defer close(h.done)

	conn, err := dialWithRetry(ctx, h.url)
	if err != nil {
		h.logger.Error("stream hub failed to connect upstream", "url", h.url, "error", err)
		return
	}
	defer conn.Close()

	stopWatcher := make(chan struct{})
	defer close(stopWatcher)
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-stopWatcher:
		}
	}()

	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			if ctx.Err() == nil {
				h.logger.Warn("stream hub upstream read error", "url", h.url, "error", err)
			}
			return
		}
		h.broadcast(data)
	}
}

func (h *Hub) broadcast(data []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.subs {
		select {
		case ch <- data:
		default:
			// Slow subscriber: drop this message for them rather than block the whole hub.
		}
	}
}

func dialWithRetry(ctx context.Context, url string) (*websocket.Conn, error) {
	var lastErr error
	for i := 0; i < dialRetries; i++ {
		conn, _, err := websocket.DefaultDialer.DialContext(ctx, url, nil)
		if err == nil {
			return conn, nil
		}
		lastErr = err
		select {
		case <-time.After(dialRetryDelay):
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}
	return nil, lastErr
}
