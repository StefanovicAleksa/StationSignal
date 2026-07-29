// Package domain holds network's pure vocabulary: the box's static IPv4 configuration, the
// fixed recovery address every box carries permanently, and validation. No I/O, no imports of
// this feature's data/service layers.
package domain

import (
	"fmt"
	"net"
	"time"
)

// RecoveryAddress is the fixed, permanent secondary address every box carries on its LAN
// interface — set up once at deploy time (see deploy/setup.sh) and never touched by this
// feature. It's the belt-and-suspenders backstop: no matter what primary address a technician
// submits, or whether the auto-revert below ever fires, the box stays reachable here. Kept in
// this package purely so the status endpoint can echo it back for display.
const RecoveryAddress = "169.254.1.1"

// DefaultRevertTimeoutSeconds is how long a provisionally-applied change waits for
// confirmation before the OS-level watchdog auto-reverts it, if config.go's own value isn't
// set.
const DefaultRevertTimeoutSeconds = 90

// recoveryBlock is the reserved link-local range RecoveryAddress lives in — a submitted primary
// address may not land in this block, or it could collide with (or shadow) the one guaranteed
// recovery path.
var recoveryBlock = mustParseCIDR("169.254.0.0/16")

func mustParseCIDR(s string) *net.IPNet {
	_, n, err := net.ParseCIDR(s)
	if err != nil {
		panic(err)
	}
	return n
}

// Config is a static IPv4 configuration for the box's LAN interface.
type Config struct {
	CIDR    string  `json:"cidr"`
	Gateway *string `json:"gateway,omitempty"`
}

// Validate checks that CIDR/Gateway are well-formed IPv4 values and that the requested primary
// address doesn't collide with the reserved recovery block.
func (c Config) Validate() error {
	ip, ipnet, err := net.ParseCIDR(c.CIDR)
	if err != nil || ip.To4() == nil {
		return &Error{Code: ErrInvalidArgument, Message: fmt.Sprintf("cidr %q is not a valid IPv4 CIDR", c.CIDR)}
	}
	ones, bits := ipnet.Mask.Size()
	if bits != 32 || ones < 1 || ones > 30 {
		return &Error{Code: ErrInvalidArgument, Message: "cidr prefix length must be between /1 and /30"}
	}
	if recoveryBlock.Contains(ip) {
		return &Error{Code: ErrInvalidArgument, Message: "address must not be in the 169.254.0.0/16 block reserved for the fixed recovery address"}
	}
	if c.Gateway != nil && *c.Gateway != "" {
		gw := net.ParseIP(*c.Gateway)
		if gw == nil || gw.To4() == nil {
			return &Error{Code: ErrInvalidArgument, Message: fmt.Sprintf("gateway %q is not a valid IPv4 address", *c.Gateway)}
		}
	}
	return nil
}

// PendingChange describes a provisionally-applied, not-yet-confirmed network change.
type PendingChange struct {
	New       Config    `json:"new"`
	ExpiresAt time.Time `json:"expiresAt"`
}

// Status is the full current-state response for GET /api/settings/network.
type Status struct {
	Interface       string         `json:"interface"`
	Current         Config         `json:"current"`
	RecoveryAddress string         `json:"recoveryAddress"`
	Pending         *PendingChange `json:"pending,omitempty"`
}
