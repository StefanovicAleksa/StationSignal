package rest

import (
	"context"
	"io"

	reportingdomain "ied_reporter_api/internal/features/reporting/domain"
	scanningdomain "ied_reporter_api/internal/features/scanning/domain"
)

type mockReportingService struct {
	startDevice reportingdomain.Device
	startErr    error
	stopErr     error
	list        []reportingdomain.Device

	gotStopID int
}

func (m *mockReportingService) Start(ctx context.Context, params reportingdomain.StartParams) (reportingdomain.Device, error) {
	return m.startDevice, m.startErr
}

func (m *mockReportingService) Stop(ctx context.Context, deviceID int) error {
	m.gotStopID = deviceID
	return m.stopErr
}

func (m *mockReportingService) List() []reportingdomain.Device {
	return m.list
}

type mockScanningService struct {
	startScan scanningdomain.Scan
	startErr  error
	stopErr   error
	list      []scanningdomain.Scan

	gotStopID int
}

func (m *mockScanningService) Start(ctx context.Context, params scanningdomain.StartParams) (scanningdomain.Scan, error) {
	return m.startScan, m.startErr
}

func (m *mockScanningService) Stop(ctx context.Context, scanID int) error {
	m.gotStopID = scanID
	return m.stopErr
}

func (m *mockScanningService) List() []scanningdomain.Scan {
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
	return New(reporting, scanning, sup, daemon, &mockStructureFileStore{}, nil)
}
