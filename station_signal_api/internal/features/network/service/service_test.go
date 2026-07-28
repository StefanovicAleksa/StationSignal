package service

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"station_signal_api/internal/features/network/domain"
)

type fakeRunner struct {
	applyErr   error
	confirmErr error
	revertErr  error

	applyCalls   int
	confirmCalls int
	revertCalls  int
	gotCIDR      string
	gotGateway   string
	gotTimeout   int
}

func (f *fakeRunner) Apply(ctx context.Context, cidr, gateway string, timeoutSeconds int) error {
	f.applyCalls++
	f.gotCIDR = cidr
	f.gotGateway = gateway
	f.gotTimeout = timeoutSeconds
	return f.applyErr
}

func (f *fakeRunner) Confirm(ctx context.Context) error {
	f.confirmCalls++
	return f.confirmErr
}

func (f *fakeRunner) Revert(ctx context.Context) error {
	f.revertCalls++
	return f.revertErr
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
