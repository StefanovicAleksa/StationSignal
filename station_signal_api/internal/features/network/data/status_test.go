package data

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestParseGlobalAddrShow(t *testing.T) {
	out := "2: eth0    inet 192.168.1.50/24 brd 192.168.1.255 scope global eth0\\       valid_lft forever preferred_lft forever\n" +
		"2: eth0    inet 169.254.1.1/24 brd 169.254.1.255 scope global eth0\\       valid_lft forever preferred_lft forever\n" +
		"3: wlan0    inet 10.0.0.7/24 brd 10.0.0.255 scope global wlan0\\       valid_lft forever preferred_lft forever\n"

	got := parseGlobalAddrShow(out)

	// The 169.254.1.1/24 (recovery block) line must be excluded.
	assert.Equal(t, []ifaceAddr{
		{Iface: "eth0", CIDR: "192.168.1.50/24"},
		{Iface: "wlan0", CIDR: "10.0.0.7/24"},
	}, got)
}

func TestParseGlobalAddrShow_EmptyOutput(t *testing.T) {
	assert.Empty(t, parseGlobalAddrShow(""))
}

func TestParseGlobalAddrShow_OnlyRecoveryAddress(t *testing.T) {
	out := "2: eth0    inet 169.254.1.1/24 brd 169.254.1.255 scope global eth0\n"
	assert.Empty(t, parseGlobalAddrShow(out))
}

func TestChooseInterface_SingleCandidate(t *testing.T) {
	candidates := []ifaceAddr{{Iface: "eth0", CIDR: "192.168.1.50/24"}}

	got, err := chooseInterface(candidates, "")

	assert.NoError(t, err)
	assert.Equal(t, candidates[0], got)
}

func TestChooseInterface_NoCandidates(t *testing.T) {
	_, err := chooseInterface(nil, "")
	assert.Error(t, err)
}

func TestChooseInterface_MultipleCandidates_DisambiguatedByRouteGet(t *testing.T) {
	candidates := []ifaceAddr{
		{Iface: "eth0", CIDR: "192.168.1.50/24"},
		{Iface: "wlan0", CIDR: "10.0.0.7/24"},
	}
	routeGet := "1.1.1.1 via 10.0.0.1 dev wlan0 src 10.0.0.7 uid 1000"

	got, err := chooseInterface(candidates, routeGet)

	assert.NoError(t, err)
	assert.Equal(t, candidates[1], got)
}

func TestChooseInterface_MultipleCandidates_RouteGetFailedFallsBackToFirst(t *testing.T) {
	candidates := []ifaceAddr{
		{Iface: "eth0", CIDR: "192.168.1.50/24"},
		{Iface: "wlan0", CIDR: "10.0.0.7/24"},
	}

	got, err := chooseInterface(candidates, "")

	assert.NoError(t, err)
	assert.Equal(t, candidates[0], got)
}

func TestParseDefaultGateway_Present(t *testing.T) {
	out := "default via 192.168.1.1 dev eth0 proto dhcp metric 100\n"
	assert.Equal(t, "192.168.1.1", parseDefaultGateway(out, "eth0"))
}

func TestParseDefaultGateway_AbsentOnIsolatedLAN(t *testing.T) {
	assert.Equal(t, "", parseDefaultGateway("", "eth0"))
}

func TestParseDefaultGateway_WrongInterfaceIgnored(t *testing.T) {
	out := "default via 192.168.1.1 dev wlan0 proto dhcp metric 100\n"
	assert.Equal(t, "", parseDefaultGateway(out, "eth0"))
}

func TestParseDefaultGateway_NoIfaceHintReturnsFirst(t *testing.T) {
	out := "default via 192.168.1.1 dev eth0 proto dhcp metric 100\n"
	assert.Equal(t, "192.168.1.1", parseDefaultGateway(out, ""))
}
