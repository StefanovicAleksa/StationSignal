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
onDeviceFound(void* userParam, uint64_t scanId, const char* host, int mmsPort) {
    (void) userParam;
    printf("[scan] found %s:%d (scan #%llu)\n", host, mmsPort, (unsigned long long) scanId);
}

/*
 * Temporary diagnostic aid (see CLAUDE.md's ied_model_online_loader bullet /
 * the plans these landed under) - mms_report_client_report_adapter.c,
 * ipc_dispatcher_mms_adapter.c, and ied_model_online_loader_connection.c each
 * append to one of these paths via their own small appendDebugLog helper
 * (fopen(path, "a")), so without this, every run's log lines pile up on top
 * of whatever a previous run already wrote, making it hard to tell current
 * output from stale output. Truncating each path once here, before anything
 * else runs, gives every process run a fresh log - the per-call helpers stay
 * simple append-only ("a") for the rest of that run. Paths are duplicated
 * string literals (not shared constants) matching every other debug-log
 * duplication already in this codebase - see the plan/CLAUDE.md for that
 * convention. Failing to truncate (e.g. no write permission) isn't fatal -
 * the daemon just falls back to appending onto a stale file, same as before
 * this existed.
 */
static void
truncateDebugLogs(void) {
    static const char* paths[] = {
        "/tmp/ied_reporter_debug_mms_before.log",
        "/tmp/ied_reporter_debug_mms_after.log",
        "/tmp/ied_reporter_debug_mms_websocket.log",
        "/tmp/ied_reporter_debug_model_build.log",
        "/tmp/ied_reporter_debug_decompose.log",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE* f = fopen(paths[i], "w");
        if (f) fclose(f);
    }
}

int
main(void) {
    truncateDebugLogs();

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
