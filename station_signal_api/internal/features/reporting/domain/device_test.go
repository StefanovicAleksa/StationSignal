package domain

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestEffectiveMMSPort(t *testing.T) {
	tests := []struct {
		name    string
		mmsPort int
		want    int
	}{
		{name: "zero falls back to default", mmsPort: 0, want: DefaultMMSPort},
		{name: "explicit value passes through", mmsPort: 61850, want: 61850},
		{name: "default value itself passes through", mmsPort: DefaultMMSPort, want: DefaultMMSPort},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			assert.Equal(t, tt.want, EffectiveMMSPort(tt.mmsPort))
		})
	}
}
