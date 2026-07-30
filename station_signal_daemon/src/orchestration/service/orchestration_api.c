#include <stdlib.h>
#include <string.h>
#include "orchestration/service/orchestration_api.h"
#include "orchestration/domain/orchestration_usecases.h"
#include "orchestration/data/orchestration_staging.h"

void
OrchestrationConfig_defaults(OrchestrationConfig* config) {
    if (!config) return;

    IpcDispatcherConfig_defaults(&config->ipcDispatcherConfig);
    SclBootstrapConfig_defaults(&config->bootstrapConfig);
    MmsReportClientConfig_defaults(&config->reportClientConfig);
    GooseSubscriberConfig_defaults(&config->gooseSubscriberConfig);
}

OrchestrationHandle
Orchestration_create(const OrchestrationConfig* config, OrchestrationError* outError) {
    OrchestrationHandle handle = calloc(1, sizeof(struct sOrchestrationHandle));
    if (!handle) {
        if (outError) *outError = ORCHESTRATION_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config) {
        handle->config = *config;
    } else {
        OrchestrationConfig_defaults(&handle->config);
    }

    /* Allocation only (ring buffer) - no I/O, matches this function's own
     * "no I/O at create" contract. The real bind happens at Orchestration_run's
     * first stage. */
    IpcDispatcherError ipcErr;
    handle->ipcDispatcher = IpcDispatcher_create(&handle->config.ipcDispatcherConfig, &ipcErr);
    if (!handle->ipcDispatcher) {
        free(handle);
        if (outError) *outError = ORCHESTRATION_ERR_IPC_DISPATCHER_FAILED;
        return NULL;
    }

    if (outError) *outError = ORCHESTRATION_OK;
    return handle;
}

void
Orchestration_setReportConnStateCallback(OrchestrationHandle handle,
        MmsReportClientConnStateCallback callback, void* userParam) {
    if (!handle) return;
    handle->connStateCallback = callback;
    handle->connStateCallbackParam = userParam;
}

void
Orchestration_wireConnStatusToIpcDispatcher(OrchestrationHandle handle) {
    if (!handle) return;
    Orchestration_setReportConnStateCallback(handle, IpcDispatcher_onConnStateChange, handle->ipcDispatcher);
}

void
Orchestration_setRcbStatusCallback(OrchestrationHandle handle,
        MmsReportClientRcbStatusCallback callback, void* userParam) {
    if (!handle) return;
    handle->rcbStatusCallback = callback;
    handle->rcbStatusCallbackParam = userParam;
}

void
Orchestration_setGooseStatusCallback(OrchestrationHandle handle,
        GooseSubscriberStatusCallback callback, void* userParam) {
    if (!handle) return;
    handle->gooseStatusCallback = callback;
    handle->gooseStatusCallbackParam = userParam;
}

void
Orchestration_setBootstrapProgressCallback(OrchestrationHandle handle,
        SclBootstrapProgressCallback callback, void* userParam) {
    if (!handle) return;
    handle->bootstrapProgressCallback = callback;
    handle->bootstrapProgressCallbackParam = userParam;
}

/*
 * Shared tail for Orchestration_run, Orchestration_runFromLocalFile, and
 * Orchestration_runFromOnlineDiscovery: mms_report_client start,
 * goose_subscriber start, fail-hard rollback - identical regardless of
 * whether iedModel came from SCL parsing or live discovery, since both
 * long-running workers only ever consume it through ied_model's own public
 * accessors. Assumes ipc_dispatcher is already started (every caller's own
 * stage 0). Takes ownership of iedModel - on ANY failure here, iedModel is
 * released and ipc_dispatcher is rolled back before returning.
 *
 * A device whose SCL declares zero targets for ONE of the two protocols is
 * no longer fail-hard: MMS_REPORT_CLIENT_ERR_NO_TARGETS /
 * GOOSE_SUBSCRIBER_ERR_NO_TARGETS specifically mean "this protocol isn't
 * present on this device", not a real failure, so that protocol is simply
 * skipped (its out-param set false) and the other one still starts normally.
 * Every other error from either _start() call remains fail-hard exactly as
 * before. Only when BOTH end up skipped - nothing left to monitor at all -
 * does this function fail, with ORCHESTRATION_ERR_NO_CAPABILITIES.
 */
static OrchestrationError
runFromIedModelHandle(OrchestrationHandle handle, IedModelHandle iedModel, const char* host, int port,
        const char* interfaceId, OrchestrationErrorDetail* outDetail,
        bool* outMmsAvailable, bool* outGooseAvailable) {
    /* --- mms_report_client, against the caller-supplied host/port (the
     * winning scl_bootstrap candidate's own host/port, the caller's own
     * argument for the local-file path, or the same host/port the online
     * discovery connection itself just used) - MmsReportClient_create copies
     * host internally. Report callback is always, unconditionally wired to
     * ipc_dispatcher - see orchestration_api.h's note on why there is no
     * caller-facing setter for this slot. */
    MmsReportClientError reportErr;
    MmsReportClientHandle reportClient = MmsReportClient_create(iedModel, host, port,
            &handle->config.reportClientConfig, &reportErr);

    if (!reportClient) {
        IedModel_release(iedModel);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_REPORT_CLIENT_START;
            outDetail->reportClientError = reportErr;
        }
        return ORCHESTRATION_ERR_REPORT_CLIENT_FAILED;
    }

    MmsReportClient_setReportCallback(reportClient, IpcDispatcher_onMmsReport, handle->ipcDispatcher);
    if (handle->connStateCallback) {
        MmsReportClient_setConnectionStateCallback(reportClient, handle->connStateCallback,
                handle->connStateCallbackParam);
    }
    if (handle->rcbStatusCallback) {
        MmsReportClient_setRcbStatusCallback(reportClient, handle->rcbStatusCallback,
                handle->rcbStatusCallbackParam);
    }

    reportErr = MmsReportClient_start(reportClient);
    bool mmsAvailable = true;
    if (reportErr != MMS_REPORT_CLIENT_OK) {
        MmsReportClient_destroy(reportClient);
        reportClient = NULL;

        if (reportErr != MMS_REPORT_CLIENT_ERR_NO_TARGETS) {
            IedModel_release(iedModel);
            IpcDispatcher_stop(handle->ipcDispatcher);
            if (outDetail) {
                outDetail->stage = ORCHESTRATION_STAGE_REPORT_CLIENT_START;
                outDetail->reportClientError = reportErr;
            }
            return ORCHESTRATION_ERR_REPORT_CLIENT_FAILED;
        }
        mmsAvailable = false;
    }

    /* --- goose_subscriber - fail-hard (except for the same "no targets"
     * carve-out as above): a real failure here tears down the report client
     * (if it started) and ipc_dispatcher too, leaving the handle clean and
     * re-runnable rather than half-started. Record callback unconditionally
     * wired to ipc_dispatcher, same as above. */
    GooseSubscriberError gooseErr;
    GooseSubscriberHandle gooseHandle = GooseSubscription_create(iedModel, interfaceId,
            &handle->config.gooseSubscriberConfig, &gooseErr);
    if (!gooseHandle) {
        if (reportClient) MmsReportClient_destroy(reportClient);
        IedModel_release(iedModel);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_GOOSE_SUBSCRIBER_START;
            outDetail->gooseSubscriberError = gooseErr;
        }
        return ORCHESTRATION_ERR_GOOSE_SUBSCRIBER_FAILED;
    }

    GooseSubscription_setRecordCallback(gooseHandle, IpcDispatcher_onGooseRecord, handle->ipcDispatcher);
    if (handle->gooseStatusCallback) {
        GooseSubscription_setStatusCallback(gooseHandle, handle->gooseStatusCallback,
                handle->gooseStatusCallbackParam);
    }

    gooseErr = GooseSubscription_start(gooseHandle);
    bool gooseAvailable = true;
    if (gooseErr != GOOSE_SUBSCRIBER_OK) {
        GooseSubscription_destroy(gooseHandle);
        gooseHandle = NULL;

        if (gooseErr != GOOSE_SUBSCRIBER_ERR_NO_TARGETS) {
            if (reportClient) MmsReportClient_destroy(reportClient);
            IedModel_release(iedModel);
            IpcDispatcher_stop(handle->ipcDispatcher);
            if (outDetail) {
                outDetail->stage = ORCHESTRATION_STAGE_GOOSE_SUBSCRIBER_START;
                outDetail->gooseSubscriberError = gooseErr;
            }
            return ORCHESTRATION_ERR_GOOSE_SUBSCRIBER_FAILED;
        }
        gooseAvailable = false;
    }

    if (!reportClient && !gooseHandle) {
        IedModel_release(iedModel);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_NO_CAPABILITIES;
        }
        return ORCHESTRATION_ERR_NO_CAPABILITIES;
    }

    handle->iedModel = iedModel;
    handle->reportClient = reportClient;
    handle->gooseSubscriber = gooseHandle;
    handle->running = true;

    if (outMmsAvailable) *outMmsAvailable = mmsAvailable;
    if (outGooseAvailable) *outGooseAvailable = gooseAvailable;

    return ORCHESTRATION_OK;
}

/*
 * Shared continuation for both Orchestration_run (network-fetched SCL) and
 * Orchestration_runFromLocalFile (caller-supplied SCL): IED-name
 * auto-detection, ied_model load, then runFromIedModelHandle's shared tail -
 * identical regardless of where sclPath's bytes came from. Assumes
 * ipc_dispatcher is already started (both callers' own stage 0) and rolls it
 * back on any failure here.
 *
 * sclPathIsOwnedTempFile: true for the scl_bootstrap path (sclPath is a
 * heap-allocated mkstemp path this function must unlink+free once
 * IedModel_loadFromFile has parsed it into memory - it has zero purpose
 * afterward, see the original stage 3 comment this preserves); false for the
 * local-file path (sclPath is the caller's own file, never touched beyond
 * reading it).
 */
static OrchestrationError
runFromSclFile(OrchestrationHandle handle, const char* sclPath, bool sclPathIsOwnedTempFile,
        const char* host, int port, const char* iedName, const char* interfaceId,
        AccessMode accessMode, OrchestrationErrorDetail* outDetail,
        bool* outMmsAvailable, bool* outGooseAvailable) {
    /* --- IED-name auto-detection - only entered when iedName is empty. No
     * interactive retry for the ambiguous (0 or >1 <IED>) case; see this
     * feature's own Architecture bullet in CLAUDE.md. */
    const char* resolvedIedName = iedName;
    char* autoDetectedIedName = NULL;

    if (!iedName || !iedName[0]) {
        IedModelLoadError listErr;
        LinkedList iedNames = IedModel_listIedNames(sclPath, &listErr);
        int discoveredCount = iedNames ? LinkedList_size(iedNames) : 0;

        if (!iedNames || discoveredCount != 1) {
            if (iedNames) LinkedList_destroyDeep(iedNames, free);
            if (sclPathIsOwnedTempFile) {
                OrchestrationStaging_cleanup(sclPath);
                free((char*) sclPath);
            }
            IpcDispatcher_stop(handle->ipcDispatcher);
            if (outDetail) {
                outDetail->stage = ORCHESTRATION_STAGE_IED_NAME_RESOLUTION;
                outDetail->discoveredIedCount = discoveredCount;
                outDetail->iedNameListError = iedNames ? IED_MODEL_OK : listErr;
            }
            return ORCHESTRATION_ERR_IED_NAME_RESOLUTION_FAILED;
        }

        autoDetectedIedName = strdup((char*) LinkedList_getData(LinkedList_getNext(iedNames)));
        LinkedList_destroyDeep(iedNames, free);
        resolvedIedName = autoDetectedIedName;
    }

    /* ied_model - parses fully into memory in one call, so an owned temp
     * file has zero purpose afterward; clean it up immediately regardless
     * of outcome rather than deferring. A caller-supplied local file is left
     * untouched either way - it's not ours to delete. */
    IedModelLoadError modelErr;
    IedModelHandle iedModel = IedModel_loadFromFile(sclPath, resolvedIedName, accessMode, &modelErr);
    if (sclPathIsOwnedTempFile) {
        OrchestrationStaging_cleanup(sclPath);
        free((char*) sclPath);
    }
    free(autoDetectedIedName);

    if (!iedModel) {
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_MODEL_LOAD;
            outDetail->modelLoadError = modelErr;
        }
        return ORCHESTRATION_ERR_MODEL_LOAD_FAILED;
    }

    return runFromIedModelHandle(handle, iedModel, host, port, interfaceId, outDetail,
            outMmsAvailable, outGooseAvailable);
}

OrchestrationError
Orchestration_run(OrchestrationHandle handle, LinkedList hostList, int mmsPort,
        const char* iedName, const char* interfaceId, AccessMode accessMode,
        OrchestrationErrorDetail* outDetail, bool* outMmsAvailable, bool* outGooseAvailable) {
    if (outDetail) memset(outDetail, 0, sizeof(*outDetail));

    if (!handle || handle->running || !hostList || LinkedList_size(hostList) == 0
            || mmsPort <= 0 || !interfaceId || !interfaceId[0]) {
        return ORCHESTRATION_ERR_INVALID_ARGUMENT;
    }

    /* --- stage 0: ipc_dispatcher - started first, deliberately, so a
     * websocket bind failure fails fast before touching the network-facing
     * MMS/GOOSE side at all. Nothing else has started yet, so no rollback
     * beyond returning is needed here. */
    IpcDispatcherError ipcStartErr = IpcDispatcher_start(handle->ipcDispatcher);
    if (ipcStartErr != IPC_DISPATCHER_OK) {
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_IPC_DISPATCHER_START;
            outDetail->ipcDispatcherError = ipcStartErr;
        }
        return ORCHESTRATION_ERR_IPC_DISPATCHER_FAILED;
    }

    /* --- stage 1: scl_bootstrap - short-lived handle, not stored on
     * OrchestrationHandle since scanAndFetch is one-shot */
    SclBootstrapError bootstrapErr;
    SclBootstrapHandle sclHandle = SclBootstrap_create(&handle->config.bootstrapConfig, &bootstrapErr);
    if (!sclHandle) {
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_BOOTSTRAP;
            outDetail->bootstrapArgError = bootstrapErr;
        }
        return ORCHESTRATION_ERR_BOOTSTRAP_FAILED;
    }
    if (handle->bootstrapProgressCallback) {
        SclBootstrap_setProgressCallback(sclHandle, handle->bootstrapProgressCallback,
                handle->bootstrapProgressCallbackParam);
    }

    LinkedList results = SclBootstrap_scanAndFetch(sclHandle, hostList, mmsPort, &bootstrapErr);
    if (!results) {
        SclBootstrap_destroy(sclHandle);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_BOOTSTRAP;
            outDetail->bootstrapArgError = bootstrapErr;
        }
        return ORCHESTRATION_ERR_BOOTSTRAP_FAILED;
    }

    SclBootstrapResult* winner = OrchestrationUseCases_selectAndDetachFirstRetrieved(results);
    if (!winner) {
        SclBootstrapCandidateStatus lastStatus = OrchestrationUseCases_summarizeBootstrapFailure(results);
        LinkedList_destroyDeep(results, SclBootstrap_destroyResult);
        SclBootstrap_destroy(sclHandle);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_BOOTSTRAP;
            outDetail->lastCandidateStatus = lastStatus;
        }
        return ORCHESTRATION_ERR_BOOTSTRAP_FAILED;
    }
    LinkedList_destroyDeep(results, SclBootstrap_destroyResult); /* winner already detached, survives */
    SclBootstrap_destroy(sclHandle);

    /* --- stage 2: staging --- */
    int stageErrno;
    char* tempPath = OrchestrationStaging_writeTempFile(winner->fileData, winner->fileSize, &stageErrno);
    if (!tempPath) {
        SclBootstrap_destroyResult(winner);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_STAGING;
            outDetail->stagingErrno = stageErrno;
        }
        return ORCHESTRATION_ERR_STAGING_FAILED;
    }

    /* --- stages 3-5: shared with Orchestration_runFromLocalFile, see that
     * function's own comment. tempPath is freed/unlinked inside (owned). */
    OrchestrationError runErr = runFromSclFile(handle, tempPath, true, winner->host, winner->port,
            iedName, interfaceId, accessMode, outDetail, outMmsAvailable, outGooseAvailable);
    SclBootstrap_destroyResult(winner);

    return runErr;
}

OrchestrationError
Orchestration_runFromLocalFile(OrchestrationHandle handle, const char* sclFilePath, const char* host,
        int mmsPort, const char* iedName, const char* interfaceId, AccessMode accessMode,
        OrchestrationErrorDetail* outDetail, bool* outMmsAvailable, bool* outGooseAvailable) {
    if (outDetail) memset(outDetail, 0, sizeof(*outDetail));

    if (!handle || handle->running || !sclFilePath || !sclFilePath[0] || !host || !host[0]
            || mmsPort <= 0 || !interfaceId || !interfaceId[0]) {
        return ORCHESTRATION_ERR_INVALID_ARGUMENT;
    }

    /* --- stage 0: ipc_dispatcher, same as Orchestration_run's own stage 0 -
     * started first, deliberately, so a bind failure fails fast. */
    IpcDispatcherError ipcStartErr = IpcDispatcher_start(handle->ipcDispatcher);
    if (ipcStartErr != IPC_DISPATCHER_OK) {
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_IPC_DISPATCHER_START;
            outDetail->ipcDispatcherError = ipcStartErr;
        }
        return ORCHESTRATION_ERR_IPC_DISPATCHER_FAILED;
    }

    /* No scl_bootstrap/staging stage at all - sclFilePath is read directly
     * and left untouched (not ours to delete), see runFromSclFile's own
     * sclPathIsOwnedTempFile doc comment. */
    return runFromSclFile(handle, sclFilePath, false, host, mmsPort, iedName, interfaceId, accessMode,
            outDetail, outMmsAvailable, outGooseAvailable);
}

OrchestrationError
Orchestration_runFromOnlineDiscovery(OrchestrationHandle handle, const char* host, int mmsPort,
        const char* iedName, const char* interfaceId, AccessMode accessMode,
        const char* acseAuthPassword, OrchestrationErrorDetail* outDetail,
        bool* outMmsAvailable, bool* outGooseAvailable) {
    if (outDetail) memset(outDetail, 0, sizeof(*outDetail));

    if (!handle || handle->running || !host || !host[0] || mmsPort <= 0 || !interfaceId || !interfaceId[0]) {
        return ORCHESTRATION_ERR_INVALID_ARGUMENT;
    }

    /* --- stage 0: ipc_dispatcher, same as every other entry point's own
     * stage 0 - started first, deliberately, so a bind failure fails fast. */
    IpcDispatcherError ipcStartErr = IpcDispatcher_start(handle->ipcDispatcher);
    if (ipcStartErr != IPC_DISPATCHER_OK) {
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_IPC_DISPATCHER_START;
            outDetail->ipcDispatcherError = ipcStartErr;
        }
        return ORCHESTRATION_ERR_IPC_DISPATCHER_FAILED;
    }

    /* --- stage: online discovery - replaces BOOTSTRAP/STAGING/MODEL_LOAD
     * entirely. IedModelOnlineLoader_build owns its own one-shot
     * IedConnection end-to-end (connect, discover, disconnect); this layer
     * never touches IedConnection directly, matching this feature's own
     * documented "zero direct third-party includes" invariant. iedName here
     * only labels the constructed model - there is no SCL <IED> list to
     * auto-detect from over a live connection, so (unlike Orchestration_run/
     * _runFromLocalFile) an empty iedName is not a resolution stage of its
     * own, it's simply passed through as-is. */
    IedModelOnlineLoaderError loaderErr;
    IedModelHandle iedModel = IedModelOnlineLoader_build(host, mmsPort, iedName, accessMode,
            acseAuthPassword, NULL, &loaderErr);
    if (!iedModel) {
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_ONLINE_DISCOVERY;
            outDetail->onlineDiscoveryError = loaderErr;
        }
        return ORCHESTRATION_ERR_ONLINE_DISCOVERY_FAILED;
    }

    return runFromIedModelHandle(handle, iedModel, host, mmsPort, interfaceId, outDetail,
            outMmsAvailable, outGooseAvailable);
}

void
Orchestration_stop(OrchestrationHandle handle) {
    if (!handle) return;

    if (handle->gooseSubscriber) {
        GooseSubscription_destroy(handle->gooseSubscriber);
        handle->gooseSubscriber = NULL;
    }
    if (handle->reportClient) {
        MmsReportClient_destroy(handle->reportClient);
        handle->reportClient = NULL;
    }
    /* Stopped only after both producers are torn down above - guarantees no
     * more producer-thread calls can land on ipc_dispatcher once it stops.
     * Safe/no-op if IpcDispatcher_start was never reached (e.g. Orchestration_run
     * never called, or failed at an earlier stage than this). */
    IpcDispatcher_stop(handle->ipcDispatcher);
    if (handle->iedModel) {
        IedModel_release(handle->iedModel);
        handle->iedModel = NULL;
    }

    handle->running = false;
}

void
Orchestration_destroy(OrchestrationHandle handle) {
    if (!handle) return;

    Orchestration_stop(handle);
    IpcDispatcher_destroy(handle->ipcDispatcher); /* always exists once Orchestration_create succeeds */
    free(handle);
}
