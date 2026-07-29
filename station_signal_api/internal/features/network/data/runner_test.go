package data

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// newFakeHelper installs a fake `sudo` on PATH that just execs its arguments, plus a stand-in
// helper script with the given body. ScriptRunner resolves both through PATH/HelperPath, so this
// exercises the real exec path — argument passing, exit-code classification, output capture —
// without needing actual privilege.
func newFakeHelper(t *testing.T, body string) (helperPath, argsLog string) {
	t.Helper()
	dir := t.TempDir()

	sudo := filepath.Join(dir, "sudo")
	require.NoError(t, os.WriteFile(sudo, []byte("#!/bin/sh\nexec \"$@\"\n"), 0o755))

	helperPath = filepath.Join(dir, "helper.sh")
	argsLog = filepath.Join(dir, "args.log")
	script := "#!/bin/sh\necho \"$@\" >>\"" + argsLog + "\"\n" + body
	require.NoError(t, os.WriteFile(helperPath, []byte(script), 0o755))

	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	return helperPath, argsLog
}

func readLog(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	require.NoError(t, err)
	return strings.TrimSpace(string(b))
}

func TestScriptRunner_Apply_PassesArgumentsAndGatewaySentinel(t *testing.T) {
	helper, argsLog := newFakeHelper(t, "exit 0\n")
	r := NewScriptRunner(helper)

	require.NoError(t, r.Apply(context.Background(), "192.168.1.50/24", "192.168.1.1", 90))
	assert.Equal(t, "apply 192.168.1.50/24 192.168.1.1 90", readLog(t, argsLog))
}

func TestScriptRunner_Apply_EmptyGatewayBecomesHelperSentinel(t *testing.T) {
	helper, argsLog := newFakeHelper(t, "exit 0\n")
	r := NewScriptRunner(helper)

	require.NoError(t, r.Apply(context.Background(), "192.168.1.50/24", "", 90))
	assert.Equal(t, "apply 192.168.1.50/24 - 90", readLog(t, argsLog))
}

// The helper's dedicated exit code has to be recognized as its own condition — otherwise "a
// change is already pending" reaches the technician as a 500 with raw shell stderr in it, rather
// than a 409 with a recovery action attached.
func TestScriptRunner_Apply_ExitCode3IsChangeAlreadyPending(t *testing.T) {
	helper, _ := newFakeHelper(t, "echo 'error: a network change is already pending' >&2\nexit 3\n")
	r := NewScriptRunner(helper)

	err := r.Apply(context.Background(), "192.168.1.50/24", "", 90)

	assert.True(t, errors.Is(err, ErrChangeAlreadyPending), "got %v", err)
}

func TestScriptRunner_Apply_OtherFailuresCarryHelperOutput(t *testing.T) {
	helper, _ := newFakeHelper(t, "echo 'nmcli: activation failed' >&2\nexit 4\n")
	r := NewScriptRunner(helper)

	err := r.Apply(context.Background(), "192.168.1.50/24", "", 90)

	require.Error(t, err)
	assert.False(t, errors.Is(err, ErrChangeAlreadyPending))
	assert.Contains(t, err.Error(), "nmcli: activation failed")
}

// THE REGRESSION TEST for the self-inflicted kill. This used to run with the HTTP request's
// context, so a browser navigating away mid-apply SIGKILLed the privileged helper partway
// through reconfiguring the box's network — which is the *normal* case here, since applying a
// new address takes away the very address the page is talking to.
func TestScriptRunner_CallerCancellationDoesNotKillTheHelper(t *testing.T) {
	dir := t.TempDir()
	marker := filepath.Join(dir, "finished")
	helper, _ := newFakeHelper(t, "sleep 0.3\ntouch \""+marker+"\"\nexit 0\n")
	r := NewScriptRunner(helper)

	ctx, cancel := context.WithCancel(context.Background())
	cancel() // already dead before the call, the worst case

	require.NoError(t, r.Apply(ctx, "192.168.1.50/24", "", 90))
	assert.FileExists(t, marker, "the helper must run to completion despite the caller's context being cancelled")
}

func TestScriptRunner_Status_ParsesHelperOutput(t *testing.T) {
	helper, argsLog := newFakeHelper(t, "printf 'PENDING=1\\nEXPIRES_AT=1785206763\\nNEW_CIDR=192.168.1.77/24\\nNEW_GATEWAY=192.168.1.1\\nRESULT=failed\\n'\nexit 0\n")
	r := NewScriptRunner(helper)

	st, err := r.Status(context.Background())

	require.NoError(t, err)
	assert.Equal(t, "status", readLog(t, argsLog))
	assert.True(t, st.Pending)
	assert.Equal(t, "192.168.1.77/24", st.CIDR)
	assert.Equal(t, "failed", st.LastActivation)
}

func TestScriptRunner_ConfirmRevertReconcile_InvokeTheirSubcommands(t *testing.T) {
	helper, argsLog := newFakeHelper(t, "exit 0\n")
	r := NewScriptRunner(helper)

	require.NoError(t, r.Confirm(context.Background()))
	require.NoError(t, r.Revert(context.Background()))
	require.NoError(t, r.Reconcile(context.Background()))

	assert.Equal(t, "confirm\nrevert\nreconcile", readLog(t, argsLog))
}

func TestParsePendingState(t *testing.T) {
	tests := []struct {
		name string
		out  string
		want PendingState
	}{
		{
			name: "nothing pending",
			out:  "PENDING=0\n",
			want: PendingState{},
		},
		{
			name: "full pending change",
			out:  "PENDING=1\nEXPIRES_AT=1785206763\nNEW_CIDR=192.168.1.77/24\nNEW_GATEWAY=192.168.1.1\nRESULT=ok\nAT=1785206700\n",
			want: PendingState{
				Pending:        true,
				ExpiresAt:      time.Unix(1785206763, 0),
				CIDR:           "192.168.1.77/24",
				Gateway:        "192.168.1.1",
				LastActivation: "ok",
			},
		},
		{
			// "-" is the helper's sentinel for "no gateway"; it must not surface as a literal.
			name: "gateway sentinel means none",
			out:  "PENDING=1\nNEW_CIDR=192.168.1.77/24\nNEW_GATEWAY=-\n",
			want: PendingState{Pending: true, CIDR: "192.168.1.77/24"},
		},
		{
			// Old markers (and hand-created ones) predate these fields entirely.
			name: "marker with no detail lines",
			out:  "PENDING=1\n",
			want: PendingState{Pending: true},
		},
		{
			name: "garbage expiry is ignored rather than fatal",
			out:  "PENDING=1\nEXPIRES_AT=not-a-number\n",
			want: PendingState{Pending: true},
		},
		{
			name: "empty output",
			out:  "",
			want: PendingState{},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			assert.Equal(t, tc.want, ParsePendingState(tc.out))
		})
	}
}
