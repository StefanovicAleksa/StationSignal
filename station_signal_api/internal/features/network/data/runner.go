// Package data is network's I/O layer: shelling out (via sudo) to the privileged netconfig
// helper script, and reading the LAN interface's current configuration via the unprivileged
// `ip` command. Imports domain for types; never imported by domain.
package data

import (
	"context"
	"fmt"
	"os/exec"
	"strconv"
)

// Runner executes the privileged netconfig helper's fixed command vocabulary
// (deploy/scripts/station-signal-netconfig.sh). This is the one seam that actually touches the
// box's network config — everything above it (service, rest) is plain Go, easily unit tested
// against a fake Runner.
type Runner interface {
	// Apply provisionally applies cidr (and gateway, or "" for none) as the LAN interface's new
	// primary address, alongside the untouched fixed recovery address, and schedules an
	// OS-level auto-revert (independent of this process) after timeoutSeconds. timeoutSeconds is
	// passed through explicitly rather than duplicated as a default inside the script, so the
	// OS-level watchdog and this service's own in-memory "pending" bookkeeping (service.go) never
	// drift apart.
	Apply(ctx context.Context, cidr, gateway string, timeoutSeconds int) error
	// Confirm cancels the pending auto-revert and makes the last-applied change permanent.
	Confirm(ctx context.Context) error
	// Revert restores the configuration that was active immediately before the last Apply.
	Revert(ctx context.Context) error
}

// ScriptRunner is the real Runner: it shells out to the helper script via sudo. This is the
// first and only place this API elevates privilege — mirrors the narrow, single-purpose shape
// of the daemon binary's own setcap grant (see deploy/sudoers/station-signal-netconfig).
type ScriptRunner struct {
	// HelperPath is the absolute path to station-signal-netconfig.sh.
	HelperPath string
}

// NewScriptRunner builds a ScriptRunner invoking the helper script at helperPath.
func NewScriptRunner(helperPath string) *ScriptRunner {
	return &ScriptRunner{HelperPath: helperPath}
}

func (r *ScriptRunner) Apply(ctx context.Context, cidr, gateway string, timeoutSeconds int) error {
	gw := gateway
	if gw == "" {
		gw = "-"
	}
	return r.run(ctx, "apply", cidr, gw, strconv.Itoa(timeoutSeconds))
}

func (r *ScriptRunner) Confirm(ctx context.Context) error {
	return r.run(ctx, "confirm")
}

func (r *ScriptRunner) Revert(ctx context.Context) error {
	return r.run(ctx, "revert")
}

func (r *ScriptRunner) run(ctx context.Context, args ...string) error {
	cmdArgs := append([]string{r.HelperPath}, args...)
	cmd := exec.CommandContext(ctx, "sudo", cmdArgs...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("station-signal-netconfig.sh %v: %w: %s", args, err, out)
	}
	return nil
}
