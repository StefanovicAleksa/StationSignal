//go:build integration

package testutil

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/stretchr/testify/require"
)

// Harness is a running station_signal_api process, wired to a real daemon binary, with small
// HTTP/WS client helpers for driving it exactly as a frontend would.
type Harness struct {
	t       *testing.T
	baseURL string
	wsBase  string
	cmd     *exec.Cmd
}

// StartAPI builds and spawns station_signal_api against daemonBin, waits for it to report
// healthy (daemon process running + control channel connected), and registers cleanup to
// stop it when the test ends.
func StartAPI(t *testing.T, daemonBin string) *Harness {
	t.Helper()
	apiBin := BuildAPI(t)
	addr := freeAddr(t)

	cmd := exec.Command(apiBin, "-daemon-bin", daemonBin, "-http-addr", addr)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	require.NoError(t, cmd.Start())

	h := &Harness{
		t:       t,
		baseURL: "http://" + addr,
		wsBase:  "ws://" + addr,
		cmd:     cmd,
	}
	t.Cleanup(h.stop)
	h.waitHealthy(10 * time.Second)
	return h
}

// PID returns the OS process ID of the station_signal_api process itself.
func (h *Harness) PID() int {
	return h.cmd.Process.Pid
}

// stop asks the API process to shut down gracefully (SIGTERM, which its own signal.
// NotifyContext handler turns into a graceful supervisor shutdown that terminates the real
// daemon child and waits for it to actually exit — see cmd/station_signal_api/main.go).
// SIGKILLing the API directly would orphan the daemon child instead (nothing reaps children
// of a killed parent), leaving it holding the daemon's fixed control-channel port
// (127.0.0.1:8767) for an indeterminate time and starving the next test's daemon of it.
func (h *Harness) stop() {
	if h.cmd.Process == nil {
		return
	}
	_ = h.cmd.Process.Signal(syscall.SIGTERM)

	done := make(chan error, 1)
	go func() { done <- h.cmd.Wait() }()

	select {
	case <-done:
	case <-time.After(10 * time.Second):
		_ = h.cmd.Process.Kill()
		<-done
	}
}

func freeAddr(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	addr := ln.Addr().String()
	require.NoError(t, ln.Close())
	return addr
}

// waitHealthy waits until /health reports both flags true for stableChecksNeeded
// consecutive polls (not just once) before returning — two station_signal_daemon processes
// sequentially reusing the same fixed control-channel port (127.0.0.1:8767) can hit a
// transient EADDRINUSE while the OS finishes releasing the previous one's socket, which the
// supervisor's own backoff/retry already handles, but a single healthy poll can still land
// in the brief window right before a subsequent respawn cycle. Requiring a short stable
// streak makes tests actually start once things have settled, not just once by luck.
func (h *Harness) waitHealthy(timeout time.Duration) {
	h.t.Helper()
	const stableChecksNeeded = 3
	stableChecks := 0
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		resp, err := h.Get("/health")
		if err == nil {
			var body struct {
				DaemonRunning           bool `json:"daemonRunning"`
				ControlChannelConnected bool `json:"controlChannelConnected"`
			}
			decodeErr := json.NewDecoder(resp.Body).Decode(&body)
			resp.Body.Close()
			if decodeErr == nil && body.DaemonRunning && body.ControlChannelConnected {
				stableChecks++
				if stableChecks >= stableChecksNeeded {
					return
				}
				time.Sleep(50 * time.Millisecond)
				continue
			}
		}
		stableChecks = 0
		time.Sleep(50 * time.Millisecond)
	}
	h.t.Fatal("API did not become healthy in time")
}

// Get issues a GET request against path (e.g. "/devices").
func (h *Harness) Get(path string) (*http.Response, error) {
	return http.Get(h.baseURL + path)
}

// Post issues a POST request with a JSON body against path.
func (h *Harness) Post(path string, body any) (*http.Response, error) {
	data, err := json.Marshal(body)
	if err != nil {
		return nil, err
	}
	return http.Post(h.baseURL+path, "application/json", bytes.NewReader(data))
}

// Delete issues a DELETE request against path.
func (h *Harness) Delete(path string) (*http.Response, error) {
	req, err := http.NewRequest(http.MethodDelete, h.baseURL+path, nil)
	if err != nil {
		return nil, err
	}
	return http.DefaultClient.Do(req)
}

// ReadBody reads and closes resp.Body exactly once, returning the raw bytes. Use this
// (rather than DecodeJSON) whenever the body is also needed for an assertion failure
// message — e.g. `require.Equalf(t, want, resp.StatusCode, "body: %s", body)` — since a
// message argument to an Equalf/NoErrorf/etc. call is evaluated eagerly regardless of
// whether the assertion passes, so reading the body a second time afterward (via
// DecodeJSON/BodyString) would fail with "read on closed response body".
func ReadBody(t *testing.T, resp *http.Response) []byte {
	t.Helper()
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	require.NoError(t, err)
	return data
}

// UnmarshalBody is ReadBody followed by json.Unmarshal into v — the safe replacement for
// "check status with BodyString in the message, then DecodeJSON" in one call.
func UnmarshalBody(t *testing.T, resp *http.Response, v any) []byte {
	t.Helper()
	data := ReadBody(t, resp)
	require.NoErrorf(t, json.Unmarshal(data, v), "body: %s", data)
	return data
}

// DecodeJSON reads and JSON-decodes resp.Body into v, closing the body. Only safe to use
// when nothing else needs the body afterward (including as another assertion's message
// argument) — prefer UnmarshalBody otherwise.
func DecodeJSON(t *testing.T, resp *http.Response, v any) {
	t.Helper()
	defer resp.Body.Close()
	require.NoError(t, json.NewDecoder(resp.Body).Decode(v))
}

// DecodeJSONNoFatal is DecodeJSON's non-fatal counterpart, for use inside a
// require.Eventually poll closure where a t.Fatal would abort the whole retry loop instead
// of just that one attempt.
func DecodeJSONNoFatal(resp *http.Response, v any) error {
	defer resp.Body.Close()
	return json.NewDecoder(resp.Body).Decode(v)
}

// BodyString reads resp.Body as a string, closing it. Only safe to use when nothing else
// needs the body afterward — see ReadBody's doc for why.
func BodyString(resp *http.Response) string {
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	return string(data)
}

// DialWS connects to one of this API's own websocket endpoints (path starting with "/ws/").
func (h *Harness) DialWS(t *testing.T, path string) *websocket.Conn {
	t.Helper()
	conn, _, err := websocket.DefaultDialer.DialContext(context.Background(), h.wsBase+path, nil)
	require.NoError(t, err)
	return conn
}
