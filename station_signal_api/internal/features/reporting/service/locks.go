package service

import "sync"

// keyedLocks hands out one *sync.Mutex per key, created lazily, so callers can serialize
// operations on the same physical device (host:mmsPort) without serializing operations on
// different devices behind each other's slow daemon I/O. Entries are never pruned — bounded in
// practice by the number of distinct physical IEDs a substation daemon sees in a process
// lifetime, a small number.
type keyedLocks struct {
	mu    sync.Mutex
	locks map[string]*sync.Mutex
}

func newKeyedLocks() *keyedLocks {
	return &keyedLocks{locks: make(map[string]*sync.Mutex)}
}

// Lock acquires the mutex for key, creating it if this is the first use of key, and returns the
// function to call to release it.
func (k *keyedLocks) Lock(key string) (unlock func()) {
	k.mu.Lock()
	l, ok := k.locks[key]
	if !ok {
		l = &sync.Mutex{}
		k.locks[key] = l
	}
	k.mu.Unlock()

	l.Lock()
	return l.Unlock
}
