#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include "device_manager/service/device_manager_api.h"
#include "features/control_dispatcher/service/control_dispatcher_api.h"
#include "scan_orchestration/service/scan_orchestration_api.h"

/*
 * Wiring only, no business logic (see CLAUDE.md's "Architecture" rule).
 *
 * main.c is now a pure background process-runner for the external API layer:
 * it creates device_manager + scan_orchestration + control_dispatcher, starts
 * the one always-on control websocket (default 127.0.0.1:8767), and blocks
 * until SIGINT/SIGTERM. There is no argv-driven boot-time device and no
 * interactive terminal prompt anymore (that flow, plus main_discovery_prompt.c,
 * was removed entirely) - every device/scan lifecycle action is driven
 * exclusively through the control websocket's four JSON commands:
 * START_REPORTING/STOP_REPORTING (relayed to device_manager) and
 * START_SCAN/STOP_SCAN (relayed to scan_orchestration) - see CLAUDE.md's
 * device_manager/scan_orchestration/control_dispatcher Architecture bullets
 * for the full envelope shape, error codes, and threading design.
 *
 * START_REPORTING's params (host/mmsPort/iedName/interfaceId/sclFilePath/
 * acseAuthPassword/accessMode) map directly onto DeviceManager_startReporting's
 * own parameters - including sclFilePath omitted meaning "discover the
 * device's structure automatically" (scl_bootstrap, then one automatic
 * online-discovery retry via ied_model_online_loader on
 * SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND - see device_manager's own
 * Architecture bullet for the full fallback sequence). Each started device
 * gets its own auto-assigned ipc_dispatcher websocket port from
 * device_manager's configured range (default 9000-9999), returned in the
 * START_REPORTING response.
 */

static volatile sig_atomic_t g_stopRequested = 0;

static void
onSignal(int sig) {
    (void) sig;
    g_stopRequested = 1;
}

static void
onDeviceFound(void* userParam, uint64_t scanId, const char* host, int mmsPort, bool authRequired) {
    (void) userParam;
    printf("[scan] found %s:%d (scan #%llu)%s\n", host, mmsPort, (unsigned long long) scanId,
            authRequired ? " [auth required]" : "");
}

int
main(void) {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    DeviceManagerError dmCreateErr;
    DeviceManagerHandle deviceManager = DeviceManager_create(NULL, &dmCreateErr);
    if (!deviceManager) {
        fprintf(stderr, "[CORE] DeviceManager_create failed (error %d)\n", (int) dmCreateErr);
        return EXIT_FAILURE;
    }

    ScanOrchestrationConfig scanConfig;
    ScanOrchestrationConfig_defaults(&scanConfig);

    ScanOrchestrationError scanCreateErr;
    ScanOrchestrationHandle scanOrchestration = ScanOrchestration_create(&scanConfig, &scanCreateErr);
    if (!scanOrchestration) {
        fprintf(stderr, "[CORE] ScanOrchestration_create failed (error %d)\n", (int) scanCreateErr);
        DeviceManager_destroy(deviceManager);
        return EXIT_FAILURE;
    }
    ScanOrchestration_setDeviceFoundCallback(scanOrchestration, onDeviceFound, NULL);

    ControlDispatcherError cdCreateErr;
    ControlDispatcherHandle controlDispatcher = ControlDispatcher_create(NULL, deviceManager, scanOrchestration,
            &cdCreateErr);
    if (!controlDispatcher) {
        fprintf(stderr, "[CORE] ControlDispatcher_create failed (error %d)\n", (int) cdCreateErr);
        ScanOrchestration_destroy(scanOrchestration);
        DeviceManager_destroy(deviceManager);
        return EXIT_FAILURE;
    }

    ControlDispatcherError cdStartErr = ControlDispatcher_start(controlDispatcher);
    if (cdStartErr != CONTROL_DISPATCHER_OK) {
        fprintf(stderr, "[CORE] ControlDispatcher_start failed (error %d)\n", (int) cdStartErr);
        ControlDispatcher_destroy(controlDispatcher);
        ScanOrchestration_destroy(scanOrchestration);
        DeviceManager_destroy(deviceManager);
        return EXIT_FAILURE;
    }

    ControlDispatcherConfig cdConfig;
    ControlDispatcherConfig_defaults(&cdConfig);
    printf("[CORE] control_dispatcher listening on 127.0.0.1:%u - send START_REPORTING/STOP_REPORTING/"
            "START_SCAN/STOP_SCAN JSON commands here.\n", (unsigned) cdConfig.port);

    while (!g_stopRequested) {
        pause();
    }

    printf("[CORE] Shutdown requested, stopping...\n");
    ControlDispatcher_destroy(controlDispatcher);   /* stop accepting new commands first */
    ScanOrchestration_destroy(scanOrchestration);   /* stops any still-active scans */
    DeviceManager_destroy(deviceManager);           /* drains + tears down every still-running device */
    return EXIT_SUCCESS;
}
