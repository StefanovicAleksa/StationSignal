package daemonproto

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestError_Error(t *testing.T) {
	tests := []struct {
		name string
		err  *Error
		want string
	}{
		{
			name: "nil receiver returns empty string",
			err:  nil,
			want: "",
		},
		{
			name: "formats code and message",
			err:  &Error{Code: ErrInvalidArgument, Message: "bad host"},
			want: "INVALID_ARGUMENT: bad host",
		},
		{
			name: "empty message still includes code",
			err:  &Error{Code: ErrDeviceNotFound},
			want: "DEVICE_NOT_FOUND: ",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			assert.Equal(t, tt.want, tt.err.Error())
		})
	}
}

func TestError_ImplementsErrorInterface(t *testing.T) {
	var err error = &Error{Code: ErrDaemonUnreachable, Message: "no connection"}
	assert.EqualError(t, err, "DAEMON_UNREACHABLE: no connection")
}
