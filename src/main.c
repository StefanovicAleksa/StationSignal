#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include "linked_list.h"
#include "orchestration/service/orchestration_api.h"
#include "orchestration/utils/orchestration_utils.h"
#include "features/ied_discovery/service/ied_discovery_api.h"
#include "main_discovery_prompt.h"

/*
 * Wiring only, no business logic (see CLAUDE.md's "Architecture" rule) - the
 * real sequencing lives in src/orchestration/service/orchestration_api.c.
 * No config-file system exists yet, so host/port/IED name/interface come
 * from argv: [host] [mmsPort] [iedName] [interface] [ipcPort]
 * [acseAuthPassword] [sclFilePath] - mmsPort/interface/ipcPort/
 * acseAuthPassword fall back to 102/"eth0"/8765/NULL(unauthenticated) if
 * omitted.
 *
 * host and iedName have NO hardcoded fallback (they used to default to
 * integration_tests/ied_simulator's "Reporter1" fixture at 127.0.0.1 - that
 * only ever matched the bundled test simulator and directly contradicted
 * the point of the ied_discovery flow below):
 *   - host omitted/empty: runs the interactive ied_discovery scan/manual-add/
 *     pick flow (see main_discovery_prompt.h) to find one instead of
 *     requiring the operator to already know it.
 *   - iedName omitted/empty: passed straight through as NULL/"" to
 *     Orchestration_run/_runFromLocalFile, which auto-detects it from the
 *     SCL's own <IED> element(s) (works only if the SCL declares exactly one).
 *
 * sclFilePath (argv[7], optional): if supplied, the daemon skips
 * scl_bootstrap entirely and loads this local file instead
 * (Orchestration_runFromLocalFile) - for IEDs/simulators (e.g. OMICRON IED
 * Scout's "Simulate IED" mode) whose MMS server doesn't implement file
 * services, so scl_bootstrap can never fetch an SCL from them even though
 * they're otherwise a real, connectable MMS/GOOSE device. host/mmsPort still
 * drive the real live connection - ied_discovery's scan/manual-add still
 * works exactly the same beforehand to find/confirm that host.
 *
 * If sclFilePath is NOT given and Orchestration_run's own scl_bootstrap stage
 * comes back with SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND (associated fine,
 * no SCL file anywhere in its file directory - the same device class
 * sclFilePath exists for, just without an operator having a local copy to
 * hand in), this file automatically retries once via
 * Orchestration_runFromOnlineDiscovery, which builds the model directly from
 * the device's own live MMS data model instead of any SCL file at all - see
 * ied_model_online_loader's own Architecture bullet in CLAUDE.md. This is the
 * one deliberate exception to CLAUDE.md's "no over-the-wire tree discovery"
 * Hard Rule; it never runs unless file-based SCL acquisition has already,
 * genuinely failed this exact way.
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

static const char*
sclCandidateStatusToString(SclBootstrapCandidateStatus status) {
    switch (status) {
        case SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER: return "no MMS server (TCP never connected)";
        case SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED: return "MMS association/browse/download failed";
        case SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND:
            return "associated fine, but no SCL file (.icd/.cid/.scd/.ssd/.sed) found in its file directory";
        case SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED: return "access denied (auth required/rejected)";
        case SCL_BOOTSTRAP_CANDIDATE_DOWNLOAD_FAILED: return "SCL file found but download failed";
        case SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED: return "file retrieved (unexpected here)";
        default: return "unknown";
    }
}

/* Prints the stage-specific diagnostic field(s) OrchestrationErrorDetail carries -
 * the generic OrchestrationUtils_errorToString(runError) alone doesn't say
 * *why* a stage failed, only *which* stage did. */
static void
printRunFailureDetail(const OrchestrationErrorDetail* detail) {
    switch (detail->stage) {
        case ORCHESTRATION_STAGE_BOOTSTRAP:
            fprintf(stderr, "  bootstrap detail: %s\n", sclCandidateStatusToString(detail->lastCandidateStatus));
            break;
        case ORCHESTRATION_STAGE_IED_NAME_RESOLUTION:
            fprintf(stderr, "  IED name auto-detection found %d <IED> element(s) in the SCL "
                    "(need exactly 1 - pass an explicit iedName instead)\n", detail->discoveredIedCount);
            break;
        case ORCHESTRATION_STAGE_MODEL_LOAD:
            fprintf(stderr, "  model load error: %d\n", (int) detail->modelLoadError);
            break;
        case ORCHESTRATION_STAGE_ONLINE_DISCOVERY:
            fprintf(stderr, "  online discovery error: %d\n", (int) detail->onlineDiscoveryError);
            break;
        default:
            break;
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
     * unauthenticated, exactly as before this was added. argv[6] is stable
     * for the whole process lifetime, so it's safe to borrow directly into
     * both config structs below (their own doc comments describe this same
     * borrowed-then-internally-copied convention). */
    const char* acseAuthPassword = (argc > 6 && argv[6][0]) ? argv[6] : NULL;
    /* Optional - if given, skip scl_bootstrap and load this file directly
     * instead (see this file's own top comment). */
    const char* sclFilePath = (argc > 7 && argv[7][0]) ? argv[7] : NULL;

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);
    if (argc > 5) config.ipcDispatcherConfig.port = (uint16_t) atoi(argv[5]);
    /* Same physical IED authenticates both associations with the same
     * ACSE password in practice - scl_bootstrap's SCL-discovery connection
     * and mms_report_client's reporting connection are independent
     * IedConnections, so each needs it set explicitly. */
    config.bootstrapConfig.acseAuthPassword = acseAuthPassword;
    config.reportClientConfig.acseAuthPassword = acseAuthPassword;

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

    /* host omitted/empty: interactive scan/manual-add/pick via ied_discovery
     * instead of a hardcoded fallback (see this file's own top comment). */
    char* discoveredHost = NULL;
    const char* host = hostArg;
    if (!host) {
        IedDiscoveryConfig discoveryConfig;
        IedDiscoveryConfig_defaults(&discoveryConfig);
        discoveryConfig.acseAuthPassword = acseAuthPassword; /* same argv[6], same reuse
                                                                  pattern as bootstrapConfig/
                                                                  reportClientConfig above */

        IedDiscoveryError discoveryError;
        IedDiscoveryHandle discoveryHandle = IedDiscovery_create(&discoveryConfig, &discoveryError);
        if (!discoveryHandle) {
            fprintf(stderr, "[CORE] IedDiscovery_create failed (error %d)\n", (int) discoveryError);
            Orchestration_destroy(handle);
            return EXIT_FAILURE;
        }

        discoveredHost = MainDiscoveryPrompt_run(discoveryHandle, interfaceId, mmsPort);
        IedDiscovery_destroy(discoveryHandle);

        if (!discoveredHost) {
            fprintf(stderr, "[CORE] No host picked, exiting.\n");
            Orchestration_destroy(handle);
            return EXIT_FAILURE;
        }
        host = discoveredHost;
    }

    OrchestrationErrorDetail detail;
    OrchestrationError runError;

    if (sclFilePath) {
        /* Skip scl_bootstrap entirely - see this file's own top comment on
         * why (e.g. OMICRON IED Scout's simulated server not implementing
         * MMS file services). host/mmsPort still drive the real live
         * mms_report_client/goose_subscriber connections. */
        printf("[CORE] Mode: local SCL file ('%s') - no network-based SCL discovery of any kind "
                "(scl_bootstrap or online discovery) will be attempted.\n", sclFilePath);
        runError = Orchestration_runFromLocalFile(handle, sclFilePath, host, mmsPort, iedName, interfaceId,
                IED_MODEL_ACCESS_REPORT_ONLY, &detail);
    } else {
        printf("[CORE] Mode: network bootstrap (scl_bootstrap) - falling back to live online discovery "
                "only if scl_bootstrap finds an associable server with no SCL file at all.\n");
        LinkedList hostList = LinkedList_create();
        LinkedList_add(hostList, (void*) host);

        runError = Orchestration_run(handle, hostList, mmsPort, iedName, interfaceId,
                IED_MODEL_ACCESS_REPORT_ONLY, &detail);

        LinkedList_destroyStatic(hostList); /* host is stack/argv/discoveredHost-owned, not heap-owned by the list */

        /* Fallback: this exact, narrow condition (associated fine, genuinely
         * no SCL file over MMS file services) is what ied_model_online_loader
         * exists for - see this file's own top comment and
         * Orchestration_runFromOnlineDiscovery's doc comment for why this
         * stays an explicit retry here rather than a silent branch inside
         * Orchestration_run itself. */
        if (runError == ORCHESTRATION_ERR_BOOTSTRAP_FAILED
                && detail.lastCandidateStatus == SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND) {
            fprintf(stderr, "[CORE] No SCL file found over MMS file services - falling back to live "
                    "online discovery (building the model directly from %s's own MMS data model)\n", host);
            runError = Orchestration_runFromOnlineDiscovery(handle, host, mmsPort, iedName, interfaceId,
                    IED_MODEL_ACCESS_REPORT_ONLY, acseAuthPassword, &detail);
        }
    }

    if (runError != ORCHESTRATION_OK) {
        fprintf(stderr, "[CORE] Orchestration_run failed at stage %d: %s\n", detail.stage,
                OrchestrationUtils_errorToString(runError));
        printRunFailureDetail(&detail);
        Orchestration_destroy(handle);
        free(discoveredHost);
        return EXIT_FAILURE;
    }

    printf("[CORE] Orchestration running against %s:%d (IED '%s', interface '%s', ACSE auth %s). "
            "ipc_dispatcher listening on 127.0.0.1:%u. Ctrl+C to stop.\n",
            host, mmsPort, iedName ? iedName : "<auto-detected>", interfaceId,
            acseAuthPassword ? "enabled" : "disabled", (unsigned) config.ipcDispatcherConfig.port);

    while (!g_stopRequested) {
        pause();
    }

    printf("[CORE] Shutdown requested, stopping...\n");
    Orchestration_destroy(handle);
    free(discoveredHost);
    return EXIT_SUCCESS;
}
