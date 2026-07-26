package data

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestPollReady_SucceedsWhenListenerIsUp(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	defer ln.Close()

	ok := PollReady(context.Background(), ln.Addr().String(), 10*time.Millisecond, 2*time.Second)

	assert.True(t, ok)
}

func TestPollReady_SucceedsOnceListenerComesUpMidPoll(t *testing.T) {
	// Reserve a free port, close the listener, then reopen it shortly after PollReady has
	// started — exercising the retry loop, not just the first attempt.
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	addr := ln.Addr().String()
	require.NoError(t, ln.Close())

	go func() {
		time.Sleep(150 * time.Millisecond)
		ln2, err := net.Listen("tcp", addr)
		if err == nil {
			defer ln2.Close()
			time.Sleep(2 * time.Second)
		}
	}()

	ok := PollReady(context.Background(), addr, 20*time.Millisecond, 3*time.Second)

	assert.True(t, ok)
}

func TestPollReady_TimesOutWhenNothingListens(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	addr := ln.Addr().String()
	require.NoError(t, ln.Close()) // nothing listens here now

	start := time.Now()
	ok := PollReady(context.Background(), addr, 20*time.Millisecond, 200*time.Millisecond)
	elapsed := time.Since(start)

	assert.False(t, ok)
	assert.Less(t, elapsed, 2*time.Second, "should give up around the timeout, not hang")
}

func TestPollReady_ReturnsFalsePromptlyWhenCtxCanceled(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	addr := ln.Addr().String()
	require.NoError(t, ln.Close())

	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	start := time.Now()
	ok := PollReady(ctx, addr, 20*time.Millisecond, 10*time.Second)
	elapsed := time.Since(start)

	assert.False(t, ok)
	assert.Less(t, elapsed, 1*time.Second, "canceled context should abort quickly, not wait out the full timeout")
}
