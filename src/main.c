#include <stdio.h>
#include <stdlib.h>
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
 */

static volatile sig_atomic_t g_stopRequested = 0;

static void
onSignal(int sig) {
    (void) sig;
    g_stopRequested = 1;
}

static void
onReport(void* userParam, const MmsReportRecord* record) {
    (void) userParam;
    printf("[report] %s entries=%d\n", record->rcbReference ? record->rcbReference : "?", record->entryCount);
    MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
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
onGooseRecord(void* userParam, const GooseSubscriberRecord* record) {
    (void) userParam;
    printf("[goose] %s entries=%d\n", record->goCbRef ? record->goCbRef : "?", record->entryCount);
    GooseSubscription_destroyRecord((GooseSubscriberRecord*) record);
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

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(&config, &createError);
    if (!handle) {
        fprintf(stderr, "[CORE] Orchestration_create failed: %s\n", OrchestrationUtils_errorToString(createError));
        return EXIT_FAILURE;
    }

    Orchestration_setReportCallback(handle, onReport, NULL);
    Orchestration_setReportConnStateCallback(handle, onReportConnState, NULL);
    Orchestration_setRcbStatusCallback(handle, onRcbStatus, NULL);
    Orchestration_setGooseRecordCallback(handle, onGooseRecord, NULL);
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

    printf("[CORE] Orchestration running against %s:%d (IED '%s', interface '%s'). Ctrl+C to stop.\n",
            host, mmsPort, iedName, interfaceId);

    while (!g_stopRequested) {
        pause();
    }

    printf("[CORE] Shutdown requested, stopping...\n");
    Orchestration_destroy(handle);
    return EXIT_SUCCESS;
}
