// Package data is network's I/O layer: shelling out (via sudo) to the privileged netconfig
// helper script, and reading the LAN interface's current configuration via the unprivileged
// `ip` command. Imports domain for types; never imported by domain.
package data

import (
	"context"
	"errors"
	"fmt"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

// ErrChangeAlreadyPending is what Apply returns when the helper refuses because a genuine,
// not-yet-expired change is already in flight. The helper signals this with a dedicated exit
// code rather than making callers pattern-match its stderr, so the service can map it to a 409
// instead of burying it in a generic 500 (see deploy/scripts/station-signal-netconfig.sh).
var ErrChangeAlreadyPending = errors.New("a network change is already pending confirmation")

// exitCodeAlreadyPending must match EXIT_PENDING in the helper script.
const exitCodeAlreadyPending = 3

// helperTimeout bounds a single helper invocation. Generously above the helper's own worst case
// (its slow step, activation, is deliberately detached into a systemd unit) but finite, so a
// wedged nmcli can't pin an HTTP handler forever.
const helperTimeout = 60 * time.Second

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
	// drift apart. Returns ErrChangeAlreadyPending if one is already in flight.
	Apply(ctx context.Context, cidr, gateway string, timeoutSeconds int) error
	// Confirm cancels the pending auto-revert and makes the last-applied change permanent.
	Confirm(ctx context.Context) error
	// Revert restores the configuration that was active immediately before the last Apply, and
	// clears the helper's on-disk pending state even if that restore fails — it is the recovery
	// path, so it must never be blocked by the very breakage it exists to undo.
	Revert(ctx context.Context) error
	// Status reads the helper's on-disk pending state. This is the authoritative answer to "is a
	// change pending", unlike the service's in-memory copy, which any API restart loses while the
	// on-disk state survives.
	Status(ctx context.Context) (PendingState, error)
	// Reconcile clears a pending change orphaned by a reboot or crash — one whose OS-level
	// auto-revert timer no longer exists to clear it. Called once at startup.
	Reconcile(ctx context.Context) error
}

// PendingState is the helper's on-disk view of an in-flight change, as reported by `status`.
type PendingState struct {
	Pending   bool
	ExpiresAt time.Time
	CIDR      string
	Gateway   string
	// LastActivation is "ok", "failed", or "" if the last apply's detached activation hasn't
	// reported yet. A "failed" here is how the API can tell the technician that the new address
	// never came up, rather than leaving them watching a poll that will never succeed.
	LastActivation string
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
	_, err := r.run(ctx, "apply", cidr, gw, strconv.Itoa(timeoutSeconds))
	return err
}

func (r *ScriptRunner) Confirm(ctx context.Context) error {
	_, err := r.run(ctx, "confirm")
	return err
}

func (r *ScriptRunner) Revert(ctx context.Context) error {
	_, err := r.run(ctx, "revert")
	return err
}

func (r *ScriptRunner) Reconcile(ctx context.Context) error {
	_, err := r.run(ctx, "reconcile")
	return err
}

func (r *ScriptRunner) Status(ctx context.Context) (PendingState, error) {
	out, err := r.run(ctx, "status")
	if err != nil {
		return PendingState{}, err
	}
	return ParsePendingState(out), nil
}

func (r *ScriptRunner) run(ctx context.Context, args ...string) (string, error) {
	// Deliberately detached from the caller's cancellation. The caller is an HTTP handler whose
	// request context dies the moment the browser navigates away or the connection drops — which
	// is the *expected* case here, since applying a new address is about to take away the very
	// address the page is talking to. Cancelling the context SIGKILLs the privileged helper
	// mid-reconfiguration; that used to be able to leave a half-applied profile and a pending
	// marker that blocked every later change. The helper has its own rollback trap for kills it
	// can't prevent — this removes the one kill we were inflicting on ourselves. Values (request
	// IDs, etc.) are preserved; only the cancellation is dropped.
	runCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), helperTimeout)
	defer cancel()

	cmdArgs := append([]string{r.HelperPath}, args...)
	cmd := exec.CommandContext(runCtx, "sudo", cmdArgs...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		var ee *exec.ExitError
		if errors.As(err, &ee) && ee.ExitCode() == exitCodeAlreadyPending {
			return string(out), ErrChangeAlreadyPending
		}
		return string(out), fmt.Errorf("station-signal-netconfig.sh %v: %w: %s", args, err, out)
	}
	return string(out), nil
}

// ParsePendingState turns the helper's `status` output — a few KEY=VALUE lines — into a
// PendingState. Pure and exported so it's unit tested directly, matching how status.go pushes
// all its parsing into pure helpers.
func ParsePendingState(out string) PendingState {
	var st PendingState
	for _, line := range strings.Split(out, "\n") {
		key, value, found := strings.Cut(strings.TrimSpace(line), "=")
		if !found {
			continue
		}
		switch key {
		case "PENDING":
			st.Pending = value == "1"
		case "EXPIRES_AT":
			if secs, err := strconv.ParseInt(value, 10, 64); err == nil && secs > 0 {
				st.ExpiresAt = time.Unix(secs, 0)
			}
		case "NEW_CIDR":
			st.CIDR = value
		case "NEW_GATEWAY":
			// "-" is the helper's sentinel for "no gateway" (see its apply vocabulary).
			if value != "-" {
				st.Gateway = value
			}
		case "RESULT":
			st.LastActivation = value
		}
	}
	return st
}
