package domain

import (
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
)

func TestNextBackoff(t *testing.T) {
	tests := []struct {
		name string
		cur  time.Duration
		want time.Duration
	}{
		{name: "doubles below cap", cur: 200 * time.Millisecond, want: 400 * time.Millisecond},
		{name: "doubles again", cur: 400 * time.Millisecond, want: 800 * time.Millisecond},
		{name: "caps at MaxBackoff when doubling would exceed it", cur: 4 * time.Second, want: MaxBackoff},
		{name: "stays at cap once reached", cur: MaxBackoff, want: MaxBackoff},
		{name: "zero doubles to zero", cur: 0, want: 0},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			assert.Equal(t, tt.want, NextBackoff(tt.cur))
		})
	}
}
