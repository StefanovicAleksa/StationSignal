#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include "device_manager/service/device_manager_api.h"
#include "features/control_dispatcher/service/control_dispatcher_api.h"
#include "orchestration/utils/orchestration_utils.h"
#include "features/ied_discovery/service/ied_discovery_api.h"
#include "scan_orchestration/service/scan_orchestration_api.h"
#include "main_discovery_prompt.h"

/*
 * Wiring only, no business logic (see CLAUDE.md's "Architecture" rule).
 *
 * main.c's primary long-lived job is now device_manager + control_dispatcher:
 * an always-on control websocket (default 127.0.0.1:8767) accepts
 * START_REPORTING/STOP_REPORTING JSON commands for any number of
 * concurrently running devices, each auto-assigned its own ipc_dispatcher
 * websocket port from device_manager's own configured range (default
 * 9000-9999) - see CLAUDE.md's device_manager/control_dispatcher Architecture
 * bullets for the full envelope shape and threading design. This replaces
 * the old single-device-per-process model, where main.c built exactly one
 * OrchestrationHandle from argv and blocked until SIGINT.
 *
 * control_dispatcher ALSO now accepts START_SCAN/STOP_SCAN commands (a
 * client-supplied interfaceId/mmsPort, no argv involved) at any time,
 * independent of the boot-time scan flow below - both share one
 * process-lifetime ScanOrchestrationHandle, created here alongside
 * deviceManager rather than scoped to the one boot-time interactive prompt.
 *
 * No config-file system exists yet, so an OPTIONAL boot-time device still
 * comes from argv: [host] [mmsPort] [iedName] [interface] [acseAuthPassword]
 * [sclFilePath] - mmsPort/interface fall back to 102/"eth0" if omitted. This
 * boot-time device is started via the exact same DeviceManager_startReporting
 * call a control-websocket client would use, not a separate code path -
 * including the host-omitted/scan-driven flow below, which now hands its
 * picked host to DeviceManager_startReporting too instead of calling
 * Orchestration_run/_runFromLocalFile directly the way this file used to.
 *
 * host omitted/empty: runs the continuous background scan (scan_orchestration)
 * on interface, streaming discovered IEC 61850 MMS devices over its own
 * shared websocket (default port 8766) while an interactive terminal prompt
 * (main_discovery_prompt.c) lets the operator pick a device from the live,
 * growing list (or type in an extra candidate IP to verify and add) at any
 * time; picking one stops the scan before falling through to the same
 * DeviceManager_startReporting call the argv-supplied-host path uses. If no
 * host is ever picked (stdin EOF), control_dispatcher simply stays up,
 * waiting for commands - this is no longer a fatal condition, since the
 * control websocket is this daemon's real interface now, not this one
 * boot-time convenience.
 *
 * iedName omitted/empty: passed straight through - auto-detected from the
 * fetched SCL's own <IED> element(s), UNLESS sclFilePath is also given, in
 * which case device_manager requires iedName explicitly (see its own service
 * header) - a stricter contract than Orchestration_runFromLocalFile's own
 * auto-detect, deliberately, per this feature's own explicit requirement.
 *
 * sclFilePath (argv[6], optional): if supplied, skips scl_bootstrap entirely
 * (via device_manager -> Orchestration_runFromLocalFile) - see
 * device_manager's own Architecture bullet for the full fallback sequence
 * (network bootstrap, then one automatic online-discovery retry on
 * SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND) that fires when sclFilePath is
 * NOT given.
 *
 * Notable behavior change from the single-device version of this file: the
 * old 5th argv slot (an ipc_dispatcher port override) is gone - there's no
 * longer one fixed dispatcher to point an override at, since device_manager
 * owns per-device port allocation from its own configured range. The three
 * old per-device diagnostic printf passthroughs
 * (onReportConnState/onRcbStatus/onGooseStatus) are also gone - device_manager
 * creates each device's own OrchestrationHandle internally and doesn't expose
 * it to main.c, so there's no attachment point for them anymore; the control
 * websocket is the real diagnostic/status interface going forward, not
 * process-local printf.
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

static void
printStartFailure(const char* host, DeviceManagerError err, const DeviceManagerErrorDetail* detail) {
    fprintf(stderr, "[CORE] DeviceManager_startReporting(%s) failed (error %d)\n", host, (int) err);
    if (err == DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED && detail) {
        fprintf(stderr, "  orchestration stage: %s (%s)\n",
                OrchestrationUtils_stageToString(detail->orchestrationDetail.stage),
                OrchestrationUtils_errorToString(detail->orchestrationError));
        if (detail->orchestrationDetail.stage == ORCHESTRATION_STAGE_BOOTSTRAP) {
            fprintf(stderr, "  bootstrap detail: %s\n",
                    OrchestrationUtils_candidateStatusToString(detail->orchestrationDetail.lastCandidateStatus));
        }
    }
}

int
main(int argc, char** argv) {
    const char* hostArg = (argc > 1 && argv[1][0]) ? argv[1] : NULL;
    int mmsPort = (argc > 2) ? atoi(argv[2]) : 102;
    const char* iedName = (argc > 3 && argv[3][0]) ? argv[3] : NULL;
    const char* interfaceId = (argc > 4) ? argv[4] : "eth0";
    /* Optional - only needed if the target IED requires ACSE authentication.
     * NULL (the default, argv omitted) means every association attempt is
     * unauthenticated. argv[5] is stable for the whole process lifetime, so
     * it's safe to borrow directly for the boot-time DeviceManager_startReporting
     * call below (device_manager itself takes its own owned copy). */
    const char* acseAuthPassword = (argc > 5 && argv[5][0]) ? argv[5] : NULL;
    /* Optional - if given, skip scl_bootstrap and load this file directly
     * instead (see this file's own top comment). */
    const char* sclFilePath = (argc > 6 && argv[6][0]) ? argv[6] : NULL;

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    DeviceManagerError dmCreateErr;
    DeviceManagerHandle deviceManager = DeviceManager_create(NULL, &dmCreateErr);
    if (!deviceManager) {
        fprintf(stderr, "[CORE] DeviceManager_create failed (error %d)\n", (int) dmCreateErr);
        return EXIT_FAILURE;
    }

    /* Process-lifetime now (used to be scoped to the host-omitted block
     * below) - so control_dispatcher's own START_SCAN/STOP_SCAN actions can
     * drive scans at any time, independent of (and concurrently with) the
     * one boot-time interactive-picker flow below, which now just reuses
     * this same handle instead of creating/destroying its own. */
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

    /* host omitted/empty: start the continuous background scan on the
     * process-lifetime scanOrchestration handle above and let the operator
     * pick a device from its live, growing list (or type a manual IP, still
     * verified via ied_discovery directly) while the scan keeps running
     * concurrently - unchanged in spirit from before this file's
     * device_manager rewrite (see this file's own top comment), except the
     * handle itself now outlives this block instead of being torn down here. */
    char* discoveredHost = NULL;
    const char* host = hostArg;
    if (!host) {
        ScanRequest scanRequest = {
            .interfaceId = interfaceId,
            .mmsPort = mmsPort,
            .sweepIntervalMs = 0, /* use scanConfig.defaultSweepIntervalMs */
            .acseAuthPassword = acseAuthPassword
        };
        uint64_t scanId;
        ScanOrchestrationError scanStartErr = ScanOrchestration_startScan(scanOrchestration, &scanRequest, &scanId);
        if (scanStartErr != SCAN_ORCHESTRATION_OK) {
            fprintf(stderr, "[CORE] ScanOrchestration_startScan failed (error %d)\n", (int) scanStartErr);
            ControlDispatcher_destroy(controlDispatcher);
            ScanOrchestration_destroy(scanOrchestration);
            DeviceManager_destroy(deviceManager);
            return EXIT_FAILURE;
        }

        printf("[CORE] Continuous scan #%llu started on interface '%s', port %d. "
                "scan_dispatcher listening on 127.0.0.1:%u.\n",
                (unsigned long long) scanId, interfaceId, mmsPort,
                (unsigned) scanConfig.scanDispatcherConfig.port);

        /* Unchanged: manual-typed-IP verification still uses a plain
         * IedDiscoveryHandle, created exactly as before - the scan above
         * never touches this path (see main_discovery_prompt.h). */
        IedDiscoveryConfig manualVerifyConfig;
        IedDiscoveryConfig_defaults(&manualVerifyConfig);
        manualVerifyConfig.acseAuthPassword = acseAuthPassword;

        IedDiscoveryError discoveryError;
        IedDiscoveryHandle manualVerifyHandle = IedDiscovery_create(&manualVerifyConfig, &discoveryError);
        if (!manualVerifyHandle) {
            fprintf(stderr, "[CORE] IedDiscovery_create failed (error %d)\n", (int) discoveryError);
            ScanOrchestration_stopScan(scanOrchestration, scanId);
            ControlDispatcher_destroy(controlDispatcher);
            ScanOrchestration_destroy(scanOrchestration);
            DeviceManager_destroy(deviceManager);
            return EXIT_FAILURE;
        }

        discoveredHost = MainDiscoveryPrompt_run(scanOrchestration, scanId, manualVerifyHandle, mmsPort);
        IedDiscovery_destroy(manualVerifyHandle);

        printf("[CORE] Stopping scan #%llu...\n", (unsigned long long) scanId);
        /* Only stop THIS scan - scanOrchestration itself stays alive for the
         * whole process now, so control_dispatcher can still serve
         * START_SCAN/STOP_SCAN for other scans afterward. */
        ScanOrchestration_stopScan(scanOrchestration, scanId); /* may block - see its own doc comment */

        if (!discoveredHost) {
            fprintf(stderr, "[CORE] No host picked - control_dispatcher stays up, waiting for commands.\n");
        } else {
            host = discoveredHost;
        }
    }

    /* Boot-time device, if a host is known (argv-supplied or just picked
     * above) - reuses the exact same DeviceManager_startReporting path a
     * control-websocket client would use, not a separate code path. Failure
     * here is no longer fatal to the whole process - control_dispatcher is
     * this daemon's real interface now, so it keeps running regardless. */
    if (host) {
        uint64_t deviceId;
        uint16_t wsPort;
        DeviceManagerErrorDetail detail;
        DeviceManagerError startErr = DeviceManager_startReporting(deviceManager, host, mmsPort, iedName,
                interfaceId, sclFilePath, acseAuthPassword, IED_MODEL_ACCESS_REPORT_ONLY,
                &deviceId, &wsPort, &detail);
        if (startErr != DEVICE_MANAGER_OK) {
            printStartFailure(host, startErr, &detail);
        } else {
            printf("[CORE] Boot-time device #%llu running against %s:%d (IED '%s', interface '%s'). "
                    "Its own ipc_dispatcher listens on 127.0.0.1:%u. Ctrl+C to stop.\n",
                    (unsigned long long) deviceId, host, mmsPort, iedName ? iedName : "<auto-detected>",
                    interfaceId, (unsigned) wsPort);
        }
    }

    free(discoveredHost);

    while (!g_stopRequested) {
        pause();
    }

    printf("[CORE] Shutdown requested, stopping...\n");
    ControlDispatcher_destroy(controlDispatcher);   /* stop accepting new commands first */
    ScanOrchestration_destroy(scanOrchestration);   /* stops any still-active scans */
    DeviceManager_destroy(deviceManager);           /* drains + tears down every still-running device */
    return EXIT_SUCCESS;
}
