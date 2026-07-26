//go:build integration

package testutil

import (
	"net"
	"os"
	"os/exec"
	"strconv"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

// StartSimulator spawns the daemon's manual "Reporter1" IED simulator (see
// ied_reporter_daemon/integration_tests/ied_simulator/src/main.c) on a free MMS port,
// registers cleanup to stop it, and returns the port it's listening on. The simulator flips
// GGIO1.Ind1.stVal every 5s so a connected report/GOOSE stream has something to observe.
func StartSimulator(t *testing.T, simBin string) int {
	t.Helper()
	return startSimulator(t, simBin, "")
}

// StartSimulatorWithPassword is StartSimulator, but the simulator requires ACSE password
// auth (SimServer_requireAuthentication) with the given password - see main.c's own optional
// argv[2]. Lets this API's integration tests exercise a password-protected device end to end,
// out of process (unlike the daemon's own in-process C integration tests, which link
// sim_server.c directly).
func StartSimulatorWithPassword(t *testing.T, simBin string, password string) int {
	t.Helper()
	return startSimulator(t, simBin, password)
}

func startSimulator(t *testing.T, simBin string, password string) int {
	t.Helper()
	port := freeMMSPort(t)

	args := []string{strconv.Itoa(port)}
	if password != "" {
		args = append(args, password)
	}

	cmd := exec.Command(simBin, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	require.NoError(t, cmd.Start())
	t.Cleanup(func() {
		if cmd.Process != nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
	})

	waitTCPUp(t, port, 5*time.Second)
	return port
}

func freeMMSPort(t *testing.T) int {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	port := ln.Addr().(*net.TCPAddr).Port
	require.NoError(t, ln.Close())
	return port
}

func waitTCPUp(t *testing.T, port int, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	addr := "127.0.0.1:" + strconv.Itoa(port)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 100*time.Millisecond)
		if err == nil {
			conn.Close()
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("simulator did not start listening on %s in time", addr)
}
