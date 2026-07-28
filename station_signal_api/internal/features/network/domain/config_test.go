package domain

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func ptr(s string) *string { return &s }

func TestConfig_Validate_ValidCases(t *testing.T) {
	tests := []struct {
		name string
		cfg  Config
	}{
		{"no gateway", Config{CIDR: "192.168.1.50/24"}},
		{"with gateway", Config{CIDR: "10.0.0.5/24", Gateway: ptr("10.0.0.1")}},
		{"empty-string gateway treated as none", Config{CIDR: "10.0.0.5/24", Gateway: ptr("")}},
		{"narrow prefix", Config{CIDR: "172.16.0.2/30"}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			assert.NoError(t, tt.cfg.Validate())
		})
	}
}

func TestConfig_Validate_InvalidCases(t *testing.T) {
	tests := []struct {
		name string
		cfg  Config
	}{
		{"not a CIDR at all", Config{CIDR: "not-an-ip"}},
		{"missing prefix length", Config{CIDR: "192.168.1.50"}},
		{"IPv6", Config{CIDR: "2001:db8::1/64"}},
		{"prefix /0 too broad", Config{CIDR: "192.168.1.0/0"}},
		{"prefix /31 too narrow", Config{CIDR: "192.168.1.0/31"}},
		{"prefix /32 too narrow", Config{CIDR: "192.168.1.1/32"}},
		{"in the reserved recovery block", Config{CIDR: "169.254.5.5/24"}},
		{"bad gateway", Config{CIDR: "192.168.1.50/24", Gateway: ptr("not-an-ip")}},
		{"IPv6 gateway", Config{CIDR: "192.168.1.50/24", Gateway: ptr("2001:db8::1")}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.cfg.Validate()
			require.Error(t, err)
			var derr *Error
			require.ErrorAs(t, err, &derr)
			assert.Equal(t, ErrInvalidArgument, derr.Code)
		})
	}
}
