// Package streamrelay fans a single daemon-side push-only websocket stream (a per-device
// report stream, or the shared scan-result stream) out to any number of frontend
// subscribers. The frontend never dials the daemon's ports directly — wsapi subscribes to a
// Hub instead.
package streamrelay

import (
	"context"
	"encoding/json"
	"log/slog"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const (
	// DefaultBufferSize is the right size for a per-device MMS/GOOSE report hub: continuous,
	// high-frequency data where a dropped message is fine to lose (a fresher value supersedes
	// it), so a small buffer that fails fast under backpressure is the correct tradeoff.
	DefaultBufferSize = 16
	// ScanBufferSize is for the one shared scan-result Hub specifically, where that tradeoff is
	// wrong: the daemon's scan_orchestration dedupes discovered hosts in its own seen-set and
	// never resends one, so a dropped scan-result message means that device is silently gone
	// from the list until the whole scan is restarted — not superseded, just lost. Sized to
	// match the daemon's own IedDiscoveryConfig.maxHosts subnet-sweep ceiling, so even a fully
	// populated /24 sweep's worth of individually-arriving discovery events can't overflow it
	// within one scan, regardless of how slow a subscriber is to drain. The cost is negligible
	// (small JSON frames, at most this many buffered pointers per subscriber).
	ScanBufferSize = 1024

	dialRetries    = 5
	dialRetryDelay = 20 * time.Millisecond
)

// Hub dials one daemon-side stream URL and rebroadcasts every frame it reads, verbatim, to
// each current subscriber. A subscriber that can't keep up has its backlog dropped rather
// than queued — the same "no replay" semantics the daemon itself applies to its own stream
// consumers, and every drop is logged (see broadcast) so a subscriber falling behind is at
// least diagnosable instead of silently invisible. One exception: a CONNECTION_STATUS frame
// (a per-device stream's connection state, not report/GOOSE change data) is retained and
// replayed to every new subscriber — see Subscribe's own comment for why a live-only relay
// loses this frame almost every time.
type Hub struct {
	url        string
	logger     *slog.Logger
	bufferSize int

	mu       sync.Mutex
	subs     map[chan []byte]struct{}
	retained []byte // last CONNECTION_STATUS frame seen, if any; nil for the scan-result Hub

	cancel context.CancelFunc
	done   chan struct{}
}

// NewHub dials url and starts relaying immediately. Call Close when the underlying daemon
// resource (device or scan) goes away. bufferSize sizes every subscriber's own channel (see
// DefaultBufferSize/ScanBufferSize's doc comments for which one a given stream type wants).
func NewHub(ctx context.Context, url string, logger *slog.Logger, bufferSize int) *Hub {
	if logger == nil {
		logger = slog.Default()
	}
	hctx, cancel := context.WithCancel(ctx)
	h := &Hub{
		url:        url,
		logger:     logger,
		bufferSize: bufferSize,
		subs:       make(map[chan []byte]struct{}),
		cancel:     cancel,
		done:       make(chan struct{}),
	}
	go h.run(hctx)
	return h
}

// Subscribe registers a new frontend subscriber and returns its message channel plus a
// cancel function the caller must invoke exactly once when done (e.g. on client
// disconnect). The channel is closed once cancel is called or the Hub itself closes.
func (h *Hub) Subscribe() (<-chan []byte, func()) {
	ch := make(chan []byte, h.bufferSize)
	h.mu.Lock()
	h.subs[ch] = struct{}{}
	retained := h.retained
	h.mu.Unlock()

	if retained != nil {
		// A device's MMS association can (and, timed against a real IED's discovery, usually
		// does) succeed well before this subscriber — the frontend's browser WS — has even
		// been opened; without this, "connected" would be lost forever the moment it happens
		// too early, leaving the UI stuck on "Connecting..." despite the device genuinely
		// being up. Fresh channel, buffer size h.bufferSize, so this practically never hits
		// the default case, but stays non-blocking regardless.
		select {
		case ch <- retained:
		default:
		}
	}

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

	if isConnectionStatus(data) {
		h.retained = data
	}

	for ch := range h.subs {
		select {
		case ch <- data:
		default:
			// Slow subscriber: drop this message for them rather than block the whole hub.
			// Debug level, not Warn - for a report/GOOSE hub (small bufferSize, drop-under-
			// backpressure is the intended, correct behavior) this is routine and would be
			// noisy at a louder level; for the scan hub it's a real problem, but still only
			// worth surfacing to someone actively debugging, not by default.
			h.logger.Debug("stream hub dropped message for lagging subscriber", "url", h.url)
		}
	}
}

// isConnectionStatus checks a frame's own "type" field rather than assuming anything about
// message shape beyond that one field — Hub stays a relay for every other purpose (scan
// results included, which never carry this type and so never populate retained).
func isConnectionStatus(data []byte) bool {
	var probe struct {
		Type string `json:"type"`
	}
	if err := json.Unmarshal(data, &probe); err != nil {
		return false
	}
	return probe.Type == "CONNECTION_STATUS"
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
