package rest

import (
	"context"
	"io"

	networkdomain "station_signal_api/internal/features/network/domain"
	reportingdomain "station_signal_api/internal/features/reporting/domain"
	scanningdomain "station_signal_api/internal/features/scanning/domain"
)

type mockReportingService struct {
	startDevice   reportingdomain.Device
	startErr      error
	stopErr       error
	stopByAddrErr error
	list          []reportingdomain.Device

	gotStopID       int
	gotStopAddrHost string
	gotStopAddrPort int
}

func (m *mockReportingService) Start(ctx context.Context, sessionID string, params reportingdomain.StartParams) (reportingdomain.Device, error) {
	return m.startDevice, m.startErr
}

func (m *mockReportingService) Stop(ctx context.Context, sessionID string, deviceID int) error {
	m.gotStopID = deviceID
	return m.stopErr
}

func (m *mockReportingService) StopByAddress(ctx context.Context, host string, mmsPort int) error {
	m.gotStopAddrHost = host
	m.gotStopAddrPort = mmsPort
	return m.stopByAddrErr
}

func (m *mockReportingService) ListForSession(sessionID string) []reportingdomain.Device {
	return m.list
}

type mockScanningService struct {
	startScan scanningdomain.Scan
	startErr  error
	stopErr   error
	list      []scanningdomain.Scan

	gotStopID int
}

func (m *mockScanningService) Start(ctx context.Context, sessionID string, params scanningdomain.StartParams) (scanningdomain.Scan, error) {
	return m.startScan, m.startErr
}

func (m *mockScanningService) Stop(ctx context.Context, sessionID string, scanID int) error {
	m.gotStopID = scanID
	return m.stopErr
}

func (m *mockScanningService) ListForSession(sessionID string) []scanningdomain.Scan {
	return m.list
}

type mockDaemonStatus struct{ connected bool }

func (m *mockDaemonStatus) Connected() bool { return m.connected }

type mockDaemonSupervisor struct{ running bool }

func (m *mockDaemonSupervisor) Running() bool { return m.running }

type mockStructureFileStore struct {
	path string
	err  error

	gotOriginalName string
}

func (m *mockStructureFileStore) Save(originalName string, r io.Reader) (string, error) {
	m.gotOriginalName = originalName
	return m.path, m.err
}

type mockNetworkService struct {
	status     networkdomain.Status
	statusErr  error
	pending    networkdomain.PendingChange
	applyErr   error
	confirmErr error
	revertErr  error

	revertCalls    int
	gotApplyConfig networkdomain.Config
}

func (m *mockNetworkService) GetStatus(ctx context.Context) (networkdomain.Status, error) {
	return m.status, m.statusErr
}

func (m *mockNetworkService) Apply(ctx context.Context, cfg networkdomain.Config) (networkdomain.PendingChange, error) {
	m.gotApplyConfig = cfg
	return m.pending, m.applyErr
}

func (m *mockNetworkService) Confirm(ctx context.Context) error {
	return m.confirmErr
}

func (m *mockNetworkService) Revert(ctx context.Context) error {
	m.revertCalls++
	return m.revertErr
}

func newTestAPI(reporting *mockReportingService, scanning *mockScanningService, sup *mockDaemonSupervisor, daemon *mockDaemonStatus) *API {
	if reporting == nil {
		reporting = &mockReportingService{}
	}
	if scanning == nil {
		scanning = &mockScanningService{}
	}
	if sup == nil {
		sup = &mockDaemonSupervisor{}
	}
	if daemon == nil {
		daemon = &mockDaemonStatus{}
	}
	return New(reporting, scanning, sup, daemon, &mockStructureFileStore{}, &mockNetworkService{}, nil)
}
