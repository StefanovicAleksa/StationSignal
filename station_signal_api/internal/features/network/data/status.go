package data

import (
	"context"
	"fmt"
	"net"
	"os/exec"
	"regexp"
	"strings"

	"station_signal_api/internal/features/network/domain"
)

// StatusReader reads the LAN interface's current IPv4 configuration. Unlike Runner, this needs
// no privilege — `ip addr`/`ip route` are read-only for any user.
type StatusReader interface {
	Current(ctx context.Context) (iface string, cfg domain.Config, err error)
}

// IPStatusReader is the real StatusReader, backed by the `ip` command. All the actual parsing
// is pushed into pure, unit-tested helper functions below — this method is the only part that
// touches the network.
type IPStatusReader struct{}

func (IPStatusReader) Current(ctx context.Context) (string, domain.Config, error) {
	addrOut, err := exec.CommandContext(ctx, "ip", "-4", "-o", "addr", "show", "scope", "global").CombinedOutput()
	if err != nil {
		return "", domain.Config{}, fmt.Errorf("ip addr show: %w: %s", err, addrOut)
	}
	candidates := parseGlobalAddrShow(string(addrOut))

	// A route-get failure here just means "no default route" (normal on an isolated substation
	// LAN with no gateway) — not fatal, chooseInterface falls back gracefully.
	routeOut, _ := exec.CommandContext(ctx, "ip", "-4", "route", "get", "1.1.1.1").CombinedOutput()
	chosen, err := chooseInterface(candidates, string(routeOut))
	if err != nil {
		return "", domain.Config{}, err
	}

	defRouteOut, _ := exec.CommandContext(ctx, "ip", "-4", "route", "show", "default").CombinedOutput()
	gw := parseDefaultGateway(string(defRouteOut), chosen.Iface)
	var gwPtr *string
	if gw != "" {
		gwPtr = &gw
	}

	return chosen.Iface, domain.Config{CIDR: chosen.CIDR, Gateway: gwPtr}, nil
}

type ifaceAddr struct {
	Iface string
	CIDR  string
}

var (
	routeGetDevRe = regexp.MustCompile(`\bdev (\S+)`)
	recoveryBlock = mustParseCIDR("169.254.0.0/16")
)

func mustParseCIDR(s string) *net.IPNet {
	_, n, err := net.ParseCIDR(s)
	if err != nil {
		panic(err)
	}
	return n
}

// parseGlobalAddrShow parses `ip -4 -o addr show scope global` output (one line per address,
// e.g. "2: eth0    inet 192.168.1.50/24 brd 192.168.1.255 scope global eth0") into (iface, cidr)
// pairs, in output order, excluding the reserved recovery block (domain.RecoveryAddress lives
// there and is never this feature's idea of the "current" primary address).
func parseGlobalAddrShow(output string) []ifaceAddr {
	var out []ifaceAddr
	for _, line := range strings.Split(output, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 4 || fields[2] != "inet" {
			continue
		}
		iface := strings.TrimSuffix(fields[1], ":")
		cidr := fields[3]
		ip, _, err := net.ParseCIDR(cidr)
		if err != nil || recoveryBlock.Contains(ip) {
			continue
		}
		out = append(out, ifaceAddr{Iface: iface, CIDR: cidr})
	}
	return out
}

// chooseInterface picks which candidate address is "the" LAN interface this feature manages.
// With today's single-NIC deployment assumption there's normally exactly one candidate; if
// there's more than one (a future multi-NIC box), routeGetOutput (from `ip route get 1.1.1.1`)
// is used to disambiguate via its own chosen device, falling back to the first candidate if
// that lookup failed (e.g. no default route) or didn't match any candidate.
func chooseInterface(candidates []ifaceAddr, routeGetOutput string) (ifaceAddr, error) {
	if len(candidates) == 0 {
		return ifaceAddr{}, fmt.Errorf("no global (non-recovery) IPv4 address found on any interface")
	}
	if len(candidates) == 1 {
		return candidates[0], nil
	}
	if m := routeGetDevRe.FindStringSubmatch(routeGetOutput); m != nil {
		for _, c := range candidates {
			if c.Iface == m[1] {
				return c, nil
			}
		}
	}
	return candidates[0], nil
}

// parseDefaultGateway parses `ip -4 route show default` output (e.g. "default via 192.168.1.1
// dev eth0 proto dhcp metric 100"), returning the gateway for ifaceHint if present. Returns ""
// if there's no default route at all — normal for an isolated substation LAN with no gateway,
// not an error condition.
func parseDefaultGateway(output, ifaceHint string) string {
	for _, line := range strings.Split(output, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 3 || fields[0] != "default" || fields[1] != "via" {
			continue
		}
		gw := fields[2]
		if ifaceHint == "" {
			return gw
		}
		for i, f := range fields {
			if f == "dev" && i+1 < len(fields) && fields[i+1] == ifaceHint {
				return gw
			}
		}
	}
	return ""
}
