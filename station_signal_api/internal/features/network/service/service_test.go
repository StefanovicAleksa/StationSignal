package service

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/features/network/data"
	"station_signal_api/internal/features/network/domain"
)

type fakeRunner struct {
	mu sync.Mutex

	applyErr     error
	confirmErr   error
	revertErr    error
	reconcileErr error
	statusState  data.PendingState
	statusErr    error
	// applyHold, if set, blocks inside Apply until it's closed — lets a test hold the shell-out
	// open while a second Apply races it.
	applyHold chan struct{}

	applyCalls     int
	confirmCalls   int
	revertCalls    int
	reconcileCalls int
	gotCIDR        string
	gotGateway     string
	gotTimeout     int
}

func (f *fakeRunner) Apply(ctx context.Context, cidr, gateway string, timeoutSeconds int) error {
	f.mu.Lock()
	f.applyCalls++
	f.gotCIDR = cidr
	f.gotGateway = gateway
	f.gotTimeout = timeoutSeconds
	hold, err := f.applyHold, f.applyErr
	f.mu.Unlock()

	if hold != nil {
		<-hold
	}

	// Model the helper's on-disk state, which is what the service now treats as authoritative:
	// a successful apply writes the pending marker, and only confirm/revert/reconcile remove it.
	f.mu.Lock()
	defer f.mu.Unlock()
	if err == nil {
		f.statusState = data.PendingState{
			Pending:   true,
			ExpiresAt: time.Now().Add(time.Duration(timeoutSeconds) * time.Second),
			CIDR:      cidr,
			Gateway:   gateway,
		}
	}
	return err
}

func (f *fakeRunner) Confirm(ctx context.Context) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.confirmCalls++
	if f.confirmErr == nil {
		f.statusState = data.PendingState{}
	}
	return f.confirmErr
}

// Revert clears the on-disk marker even when the restore itself fails — that is the helper's
// actual contract, and the whole reason it can unstick a box.
func (f *fakeRunner) Revert(ctx context.Context) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.revertCalls++
	f.statusState = data.PendingState{}
	return f.revertErr
}

func (f *fakeRunner) Reconcile(ctx context.Context) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.reconcileCalls++
	if f.reconcileErr == nil {
		f.statusState = data.PendingState{}
	}
	return f.reconcileErr
}

func (f *fakeRunner) Status(ctx context.Context) (data.PendingState, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.statusState, f.statusErr
}

func (f *fakeRunner) counts() (apply, confirm, revert, reconcile int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.applyCalls, f.confirmCalls, f.revertCalls, f.reconcileCalls
}

type fakeStatusReader struct {
	iface string
	cfg   domain.Config
	err   error
}

func (f *fakeStatusReader) Current(ctx context.Context) (string, domain.Config, error) {
	return f.iface, f.cfg, f.err
}

func zeroCount() int { return 0 }

func newTestService(runner *fakeRunner, reader *fakeStatusReader, revertTimeout time.Duration, countReporting, countScanning func() int) *Service {
	if countReporting == nil {
		countReporting = zeroCount
	}
	if countScanning == nil {
		countScanning = zeroCount
	}
	return New(countReporting, countScanning, runner, reader, revertTimeout, nil)
}

func TestService_GetStatus_ReportsCurrentConfigAndRecoveryAddress(t *testing.T) {
	reader := &fakeStatusReader{iface: "eth0", cfg: domain.Config{CIDR: "192.168.1.50/24"}}
	svc := newTestService(&fakeRunner{}, reader, time.Second, nil, nil)

	status, err := svc.GetStatus(context.Background())

	require.NoError(t, err)
	assert.Equal(t, "eth0", status.Interface)
	assert.Equal(t, "192.168.1.50/24", status.Current.CIDR)
	assert.Equal(t, domain.RecoveryAddress, status.RecoveryAddress)
	assert.Nil(t, status.Pending)
}

func TestService_GetStatus_ReaderFailureIsApplyFailedError(t *testing.T) {
	reader := &fakeStatusReader{err: errors.New("ip: command not found")}
	svc := newTestService(&fakeRunner{}, reader, time.Second, nil, nil)

	_, err := svc.GetStatus(context.Background())

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrApplyFailed, derr.Code)
}

func TestService_Apply_InvalidConfigNeverInvokesRunner(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Second, nil, nil)

	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "garbage"})

	require.Error(t, err)
	assert.Zero(t, runner.applyCalls)
}

func TestService_Apply_RefusesWhileReportingSessionsActive(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Second, func() int { return 2 }, nil)

	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrSessionsActive, derr.Code)
	assert.Zero(t, runner.applyCalls)
}

func TestService_Apply_RefusesWhileScansActive(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Second, nil, func() int { return 1 })

	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrSessionsActive, derr.Code)
	assert.Zero(t, runner.applyCalls)
}

func TestService_Apply_Success_InvokesRunnerAndRecordsPending(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)
	gw := "192.168.1.1"

	pending, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24", Gateway: &gw})

	require.NoError(t, err)
	assert.Equal(t, 1, runner.applyCalls)
	assert.Equal(t, "192.168.1.50/24", runner.gotCIDR)
	assert.Equal(t, "192.168.1.1", runner.gotGateway)
	assert.Equal(t, 60, runner.gotTimeout, "the OS-level watchdog timeout must match this service's own revertTimeout")
	assert.Equal(t, "192.168.1.50/24", pending.New.CIDR)
	assert.True(t, pending.ExpiresAt.After(time.Now()))

	status, err := svc.GetStatus(context.Background())
	require.NoError(t, err)
	require.NotNil(t, status.Pending)
	assert.Equal(t, "192.168.1.50/24", status.Pending.New.CIDR)
}

func TestService_Apply_NoGateway_PassesEmptyStringToRunner(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})

	require.NoError(t, err)
	assert.Equal(t, "", runner.gotGateway)
}

func TestService_Apply_RunnerFailureSurfacesAsApplyFailed(t *testing.T) {
	runner := &fakeRunner{applyErr: errors.New("nmcli: connection profile not found")}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrApplyFailed, derr.Code)

	status, statusErr := svc.GetStatus(context.Background())
	require.NoError(t, statusErr)
	assert.Nil(t, status.Pending, "a failed apply must not leave a pending change behind")
}

func TestService_Apply_RefusesOverlappingChange(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)
	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})
	require.NoError(t, err)

	_, err = svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.60/24"})

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrChangeAlreadyPending, derr.Code)
	assert.Equal(t, 1, runner.applyCalls, "the second, overlapping Apply must never reach the runner")
}

func TestService_Confirm_NoPendingChangeIsRejected(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	err := svc.Confirm(context.Background())

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrNoPendingChange, derr.Code)
	assert.Zero(t, runner.confirmCalls)
}

func TestService_Confirm_Success_ClearsPendingAndAllowsNextApply(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)
	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})
	require.NoError(t, err)

	err = svc.Confirm(context.Background())
	require.NoError(t, err)

	assert.Equal(t, 1, runner.confirmCalls)
	status, err := svc.GetStatus(context.Background())
	require.NoError(t, err)
	assert.Nil(t, status.Pending)

	// A confirmed change clears the pending flag, so a further Apply is allowed again.
	_, err = svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.60/24"})
	assert.NoError(t, err)
}

func TestService_Confirm_RunnerFailureLeavesPendingIntact(t *testing.T) {
	runner := &fakeRunner{confirmErr: errors.New("systemctl stop failed")}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)
	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})
	require.NoError(t, err)

	err = svc.Confirm(context.Background())

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrApplyFailed, derr.Code)

	status, statusErr := svc.GetStatus(context.Background())
	require.NoError(t, statusErr)
	assert.NotNil(t, status.Pending, "a failed confirm must leave the pending change in place")
}

// The pending marker lives on disk and outlives this process; the in-memory copy does not. An
// API restart in the middle of a change must not make that change invisible — that divergence is
// what let a stuck change silently block every apply while the status page showed nothing.
func TestService_GetStatus_ReportsPendingChangeThisProcessNeverSaw(t *testing.T) {
	runner := &fakeRunner{statusState: data.PendingState{
		Pending:   true,
		ExpiresAt: time.Now().Add(time.Minute),
		CIDR:      "192.168.1.60/24",
		Gateway:   "192.168.1.1",
	}}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	status, err := svc.GetStatus(context.Background())

	require.NoError(t, err)
	require.NotNil(t, status.Pending)
	assert.Equal(t, "192.168.1.60/24", status.Pending.New.CIDR)
	require.NotNil(t, status.Pending.New.Gateway)
	assert.Equal(t, "192.168.1.1", *status.Pending.New.Gateway)
}

// A status page that hard-fails because the helper is unreachable is worse than one showing a
// slightly stale answer — the technician is most likely to be looking at it precisely when the
// box is unhealthy.
func TestService_GetStatus_FallsBackToMemoryWhenHelperUnreadable(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)
	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})
	require.NoError(t, err)

	runner.mu.Lock()
	runner.statusErr = errors.New("sudo: helper not found")
	runner.mu.Unlock()

	status, err := svc.GetStatus(context.Background())

	require.NoError(t, err)
	require.NotNil(t, status.Pending)
	assert.Equal(t, "192.168.1.50/24", status.Pending.New.CIDR)
}

// The helper refusing because a change is genuinely in flight is a distinct, actionable state —
// the technician can clear it from the Settings page. It must not be flattened into a generic
// APPLY_FAILED carrying raw shell stderr.
func TestService_Apply_HelperRefusalSurfacesAsChangeAlreadyPending(t *testing.T) {
	runner := &fakeRunner{applyErr: data.ErrChangeAlreadyPending}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrChangeAlreadyPending, derr.Code)
	assert.NotContains(t, derr.Message, "sudo", "the raw helper output must not leak into the message")
}

// Two applies arriving at once must not both reach the helper: the second one's "previous
// config" snapshot would capture the first one's already-modified profile, so a later revert
// would restore the wrong address. Run with -race.
func TestService_Apply_ConcurrentCallsOnlyOneReachesRunner(t *testing.T) {
	hold := make(chan struct{})
	runner := &fakeRunner{applyHold: hold}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	const callers = 8
	var wg sync.WaitGroup
	results := make(chan error, callers)
	for i := 0; i < callers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})
			results <- err
		}()
	}

	// Exactly one caller wins and parks inside the runner; every other one must come back
	// refused before we let the winner finish, which is what makes this deterministic.
	for i := 0; i < callers-1; i++ {
		err := <-results
		var derr *domain.Error
		require.ErrorAs(t, err, &derr)
		assert.Equal(t, domain.ErrChangeAlreadyPending, derr.Code)
	}

	close(hold)
	wg.Wait()
	assert.NoError(t, <-results, "the one caller that got through should have succeeded")

	applyCalls, _, _, _ := runner.counts()
	assert.Equal(t, 1, applyCalls, "only one Apply may reach the privileged helper")
}

func TestService_Revert_ClearsPendingAndAllowsNextApply(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)
	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})
	require.NoError(t, err)

	require.NoError(t, svc.Revert(context.Background()))

	_, _, revertCalls, _ := runner.counts()
	assert.Equal(t, 1, revertCalls)
	status, err := svc.GetStatus(context.Background())
	require.NoError(t, err)
	assert.Nil(t, status.Pending)

	_, err = svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.60/24"})
	assert.NoError(t, err)
}

// The case that matters most: a marker left on disk by an earlier failed auto-revert, which this
// process knows nothing about. Reverting has to work anyway — otherwise the only way out of the
// wedge is shell access to the box.
func TestService_Revert_ClearsPendingChangeThisProcessNeverSaw(t *testing.T) {
	runner := &fakeRunner{statusState: data.PendingState{Pending: true, CIDR: "192.168.1.60/24"}}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	require.NoError(t, svc.Revert(context.Background()))

	_, _, revertCalls, _ := runner.counts()
	assert.Equal(t, 1, revertCalls)

	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.70/24"})
	assert.NoError(t, err)
}

// A restore that fails still clears the marker, so the box is no longer wedged — but the
// technician must be told the previous address may not be back.
func TestService_Revert_RunnerFailureStillClearsPending(t *testing.T) {
	runner := &fakeRunner{revertErr: errors.New("nmcli: activation failed")}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)
	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.50/24"})
	require.NoError(t, err)

	err = svc.Revert(context.Background())

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrApplyFailed, derr.Code)

	_, applyErr := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.60/24"})
	assert.NoError(t, applyErr, "a failed revert must still leave the box able to try again")
}

func TestService_Revert_NothingPendingIsRejected(t *testing.T) {
	runner := &fakeRunner{}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	err := svc.Revert(context.Background())

	var derr *domain.Error
	require.ErrorAs(t, err, &derr)
	assert.Equal(t, domain.ErrNoPendingChange, derr.Code)
	_, _, revertCalls, _ := runner.counts()
	assert.Zero(t, revertCalls)
}

// If the helper's state can't be read, the recovery path still runs — refusing to attempt
// recovery because the box is in a bad state defeats the point of having it.
func TestService_Revert_RunsAnywayWhenStateUnreadable(t *testing.T) {
	runner := &fakeRunner{statusErr: errors.New("sudo: helper not found")}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	require.NoError(t, svc.Revert(context.Background()))

	_, _, revertCalls, _ := runner.counts()
	assert.Equal(t, 1, revertCalls)
}

// After a reboot the OS-level auto-revert timer is gone but the on-disk marker survives, so
// nothing would ever clear it. Startup reconciliation is what stops a box coming back up
// permanently unable to change its own address.
func TestService_Reconcile_ClearsOrphanedPendingAtStartup(t *testing.T) {
	runner := &fakeRunner{statusState: data.PendingState{Pending: true, CIDR: "192.168.1.60/24"}}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	svc.Reconcile(context.Background())

	_, _, _, reconcileCalls := runner.counts()
	assert.Equal(t, 1, reconcileCalls)
	_, err := svc.Apply(context.Background(), domain.Config{CIDR: "192.168.1.70/24"})
	assert.NoError(t, err)
}

func TestService_Reconcile_FailureIsNonFatal(t *testing.T) {
	runner := &fakeRunner{reconcileErr: errors.New("sudo: helper not found")}
	svc := newTestService(runner, &fakeStatusReader{}, time.Minute, nil, nil)

	assert.NotPanics(t, func() { svc.Reconcile(context.Background()) })
}
