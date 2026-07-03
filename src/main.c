#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include "linked_list.h"
#include "orchestration/service/orchestration_api.h"
#include "orchestration/utils/orchestration_utils.h"

/*
 * Wiring only, no business logic (see CLAUDE.md's "Architecture" rule) - the
 * real sequencing lives in src/orchestration/service/orchestration_api.c.
 * No config-file system exists yet, so host/port/IED name/interface come
 * from argv with hardcoded defaults matching integration_tests/ied_simulator's
 * "Reporter1" fixture.
 *
 * Note this file never includes features/ipc_dispatcher/service/
 * ipc_dispatcher_api.h directly - orchestration owns that feature's entire
 * lifecycle end-to-end (bind/start/stop/destroy, plus wiring its callbacks
 * onto mms_report_client/goose_subscriber internally). This file only
 * configures it via OrchestrationConfig.ipcDispatcherConfig.
 */

static volatile sig_atomic_t g_stopRequested = 0;

static void
onSignal(int sig) {
    (void) sig;
    g_stopRequested = 1;
}

static void
onReportConnState(void* userParam, MmsReportClientConnState state) {
    (void) userParam;
    printf("[mms_report_client] connection state: %d\n", state);
}

static void
onRcbStatus(void* userParam, const char* rcbReference, bool enabled, IedClientError lastError) {
    (void) userParam;
    printf("[mms_report_client] RCB %s %s (lastError=%d)\n", rcbReference ? rcbReference : "?",
            enabled ? "enabled" : "disabled", lastError);
}

static void
onGooseStatus(void* userParam, const char* goCbRef, GooseSubscriberStatus status, GooseParseError lastParseError) {
    (void) userParam;
    printf("[goose_subscriber] %s status=%d (lastParseError=%d)\n", goCbRef ? goCbRef : "?", status,
            lastParseError);
}

int
main(int argc, char** argv) {
    const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
    int mmsPort = (argc > 2) ? atoi(argv[2]) : 102;
    const char* iedName = (argc > 3) ? argv[3] : "Reporter1";
    const char* interfaceId = (argc > 4) ? argv[4] : "eth0";

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);
    if (argc > 5) config.ipcDispatcherConfig.port = (uint16_t) atoi(argv[5]);

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(&config, &createError);
    if (!handle) {
        fprintf(stderr, "[CORE] Orchestration_create failed: %s\n", OrchestrationUtils_errorToString(createError));
        return EXIT_FAILURE;
    }

    /* Report/GOOSE data records are always relayed via ipc_dispatcher,
     * wired internally by orchestration itself - no setter for those slots
     * exists here. Connection-state/RCB-status/liveness diagnostics stay on
     * printf passthroughs, out of ipc_dispatcher's v1 scope (data records
     * only). */
    Orchestration_setReportConnStateCallback(handle, onReportConnState, NULL);
    Orchestration_setRcbStatusCallback(handle, onRcbStatus, NULL);
    Orchestration_setGooseStatusCallback(handle, onGooseStatus, NULL);

    LinkedList hostList = LinkedList_create();
    LinkedList_add(hostList, (void*) host);

    OrchestrationErrorDetail detail;
    OrchestrationError runError = Orchestration_run(handle, hostList, mmsPort, iedName, interfaceId,
            IED_MODEL_ACCESS_REPORT_ONLY, &detail);

    LinkedList_destroyStatic(hostList); /* host is stack/argv-owned, not heap-owned by the list */

    if (runError != ORCHESTRATION_OK) {
        fprintf(stderr, "[CORE] Orchestration_run failed at stage %d: %s\n", detail.stage,
                OrchestrationUtils_errorToString(runError));
        Orchestration_destroy(handle);
        return EXIT_FAILURE;
    }

    printf("[CORE] Orchestration running against %s:%d (IED '%s', interface '%s'). "
            "ipc_dispatcher listening on 127.0.0.1:%u. Ctrl+C to stop.\n",
            host, mmsPort, iedName, interfaceId, (unsigned) config.ipcDispatcherConfig.port);

    while (!g_stopRequested) {
        pause();
    }

    printf("[CORE] Shutdown requested, stopping...\n");
    Orchestration_destroy(handle);
    return EXIT_SUCCESS;
}
