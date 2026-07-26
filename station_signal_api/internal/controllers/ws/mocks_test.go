package ws

import (
	"sync"
	"sync/atomic"
)

type connectedDevice struct {
	host    string
	mmsPort int
}

type mockReportingStreamer struct {
	ch           chan []byte
	ok           bool
	cancelCalled int32
	connected    []connectedDevice
}

func (m *mockReportingStreamer) StreamFor(deviceID int) (<-chan []byte, func(), bool) {
	if !m.ok {
		return nil, nil, false
	}
	var once sync.Once
	cancel := func() {
		once.Do(func() { atomic.AddInt32(&m.cancelCalled, 1) })
	}
	return m.ch, cancel, true
}

func (m *mockReportingStreamer) IsConnected(host string, mmsPort int) bool {
	for _, d := range m.connected {
		if d.host == host && d.mmsPort == mmsPort {
			return true
		}
	}
	return false
}

func (m *mockReportingStreamer) cancelWasCalled() bool {
	return atomic.LoadInt32(&m.cancelCalled) > 0
}

type mockScanningStreamer struct {
	mu           sync.Mutex
	ok           bool
	ch           chan []byte
	cancelCalled int32
}

func (m *mockScanningStreamer) StreamScans() (<-chan []byte, func(), bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.ok {
		return nil, nil, false
	}
	var once sync.Once
	cancel := func() {
		once.Do(func() { atomic.AddInt32(&m.cancelCalled, 1) })
	}
	return m.ch, cancel, true
}

func (m *mockScanningStreamer) setReady(ok bool, ch chan []byte) {
	m.mu.Lock()
	m.ok = ok
	m.ch = ch
	m.mu.Unlock()
}

func (m *mockScanningStreamer) cancelWasCalled() bool {
	return atomic.LoadInt32(&m.cancelCalled) > 0
}
