// Package daemonclient owns the single long-lived connection to the daemon's control
// channel (ws://127.0.0.1:8767) and every request/response correlation over it. All REST
// handlers in httpapi funnel their daemon calls through one shared *Client — the control
// channel broadcasts every response to every connected client, so a second connection would
// be pure overhead, not extra safety.
package daemonclient

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/google/uuid"
	"github.com/gorilla/websocket"

	"station_signal_api/internal/core/daemonproto"
)

const (
	callTimeout    = 120 * time.Second
	dialRetries    = 5
	dialRetryDelay = 200 * time.Millisecond
)

// Caller is the subset of *Client that feature data layers depend on — the seam unit tests
// mock instead of the concrete *Client. *Client satisfies this implicitly.
type Caller interface {
	Call(ctx context.Context, action string, params any) (json.RawMessage, error)
}

// Client is a single control-channel connection to the daemon, redialed each time the
// supervisor signals the daemon process has (re)started.
type Client struct {
	addr     string // e.g. "ws://127.0.0.1:8767"
	restarts <-chan struct{}
	logger   *slog.Logger

	mu        sync.Mutex
	conn      *websocket.Conn
	connected bool
	pending   map[string]chan daemonproto.Response

	writeMu sync.Mutex

	disconnected chan struct{}
	reconnects   chan struct{}
}

// New builds a Client that dials addr each time restarts fires. restarts is expected to be
// supervisor.Supervisor.Restarts() — the client never dials on its own timer, so it can never
// race the daemon process's listener coming up. restarts must have exactly one consumer
// (this Client); anything else that needs to react to a (re)connect — e.g. crash re-arm —
// should watch Reconnects() instead, which fires only once the control channel is actually
// usable again (Call will succeed), not merely once the process has restarted.
func New(addr string, restarts <-chan struct{}, logger *slog.Logger) *Client {
	if logger == nil {
		logger = slog.Default()
	}
	return &Client{
		addr:         addr,
		restarts:     restarts,
		logger:       logger,
		pending:      make(map[string]chan daemonproto.Response),
		reconnects:   make(chan struct{}, 1),
		disconnected: make(chan struct{}, 1),
	}
}

// Run blocks, (re)connecting to the control channel every time restarts fires, until ctx is
// canceled.
func (c *Client) Run(ctx context.Context) {
	for {
		select {
		case <-c.restarts:
		case <-ctx.Done():
			return
		}

		if err := c.connect(); err != nil {
			c.logger.Error("failed to connect to daemon control channel", "error", err)
			continue
		}

		select {
		case <-c.disconnected:
			c.logger.Warn("daemon control channel disconnected, awaiting next restart signal")
		case <-ctx.Done():
			c.closeConn()
			return
		}
	}
}

// Reconnects fires once after every successful (re)connection to the control channel —
// including the very first one. Unlike supervisor.Restarts(), a signal here guarantees Call
// will actually work, not just that the process is up. Crash re-arm should watch this, not
// Restarts() directly (which the Client itself is the sole consumer of).
func (c *Client) Reconnects() <-chan struct{} {
	return c.reconnects
}

// Connected reports whether the control channel is currently connected.
func (c *Client) Connected() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.connected
}

// Call sends a control-channel request and waits for its correlated response. On success it
// returns the raw "result" object. On any failure — the daemon reporting success:false, a
// transport problem, or a timeout — it returns a *daemonproto.Error so callers (httpapi) can
// map on .Code uniformly regardless of cause; transport/timeout failures use the synthetic
// daemonproto.ErrDaemonUnreachable code.
func (c *Client) Call(ctx context.Context, action string, params any) (json.RawMessage, error) {
	c.mu.Lock()
	if !c.connected {
		c.mu.Unlock()
		return nil, unreachable("not connected to daemon control channel")
	}
	conn := c.conn
	requestID := uuid.NewString()
	respCh := make(chan daemonproto.Response, 1)
	c.pending[requestID] = respCh
	c.mu.Unlock()

	defer func() {
		c.mu.Lock()
		delete(c.pending, requestID)
		c.mu.Unlock()
	}()

	data, err := json.Marshal(daemonproto.Request{RequestID: requestID, Action: action, Params: params})
	if err != nil {
		return nil, fmt.Errorf("marshal control request: %w", err)
	}

	c.writeMu.Lock()
	err = conn.WriteMessage(websocket.TextMessage, data)
	c.writeMu.Unlock()
	if err != nil {
		return nil, unreachable(fmt.Sprintf("write control request: %v", err))
	}

	callCtx, cancel := context.WithTimeout(ctx, callTimeout)
	defer cancel()

	select {
	case resp := <-respCh:
		if !resp.Success {
			if resp.Error != nil {
				return nil, resp.Error
			}
			return nil, unreachable("daemon reported failure with no error detail")
		}
		return resp.Result, nil
	case <-callCtx.Done():
		return nil, unreachable("timed out waiting for daemon response")
	}
}

func (c *Client) connect() error {
	var lastErr error
	for i := 0; i < dialRetries; i++ {
		conn, _, err := websocket.DefaultDialer.Dial(c.addr, nil)
		if err == nil {
			c.mu.Lock()
			c.conn = conn
			c.connected = true
			c.pending = make(map[string]chan daemonproto.Response)
			c.mu.Unlock()
			go c.readLoop(conn)
			c.logger.Info("connected to daemon control channel", "addr", c.addr)
			select {
			case c.reconnects <- struct{}{}:
			default:
			}
			return nil
		}
		lastErr = err
		time.Sleep(dialRetryDelay)
	}
	return lastErr
}

func (c *Client) closeConn() {
	c.mu.Lock()
	conn := c.conn
	c.connected = false
	c.mu.Unlock()
	if conn != nil {
		_ = conn.Close()
	}
}

func (c *Client) readLoop(conn *websocket.Conn) {
	defer func() {
		c.mu.Lock()
		c.connected = false
		for id, ch := range c.pending {
			delete(c.pending, id)
			select {
			case ch <- daemonproto.Response{Success: false, Error: unreachable("daemon control channel disconnected")}:
			default:
			}
		}
		c.mu.Unlock()
		_ = conn.Close()
		select {
		case c.disconnected <- struct{}{}:
		default:
		}
	}()

	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			c.logger.Warn("daemon control channel read error", "error", err)
			return
		}

		var resp daemonproto.Response
		if err := json.Unmarshal(data, &resp); err != nil {
			c.logger.Warn("malformed control channel response", "error", err)
			continue
		}

		c.mu.Lock()
		ch, ok := c.pending[resp.RequestID]
		if ok {
			delete(c.pending, resp.RequestID)
		}
		c.mu.Unlock()

		if ok {
			ch <- resp
		}
	}
}

func unreachable(msg string) *daemonproto.Error {
	return &daemonproto.Error{Code: daemonproto.ErrDaemonUnreachable, Message: msg}
}
