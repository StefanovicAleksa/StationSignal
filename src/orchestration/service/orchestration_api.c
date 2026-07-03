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

OrchestrationError
Orchestration_run(OrchestrationHandle handle, LinkedList hostList, int mmsPort,
        const char* iedName, const char* interfaceId, AccessMode accessMode,
        OrchestrationErrorDetail* outDetail) {
    if (outDetail) memset(outDetail, 0, sizeof(*outDetail));

    if (!handle || handle->running || !hostList || LinkedList_size(hostList) == 0
            || mmsPort <= 0 || !iedName || !iedName[0] || !interfaceId || !interfaceId[0]) {
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

    /* --- stage 3: ied_model - parses fully into memory in one call, so the
     * temp file has zero purpose afterward; clean it up immediately
     * regardless of outcome rather than deferring */
    IedModelLoadError modelErr;
    IedModelHandle iedModel = IedModel_loadFromFile(tempPath, iedName, accessMode, &modelErr);
    OrchestrationStaging_cleanup(tempPath);
    free(tempPath);

    if (!iedModel) {
        SclBootstrap_destroyResult(winner);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_MODEL_LOAD;
            outDetail->modelLoadError = modelErr;
        }
        return ORCHESTRATION_ERR_MODEL_LOAD_FAILED;
    }

    /* --- stage 4: mms_report_client, against the winning candidate's own
     * host/port - MmsReportClient_create copies host internally, so winner
     * only needs to survive this one call. Report callback is always,
     * unconditionally wired to ipc_dispatcher - see orchestration_api.h's
     * note on why there is no caller-facing setter for this slot. */
    MmsReportClientError reportErr;
    MmsReportClientHandle reportClient = MmsReportClient_create(iedModel, winner->host, winner->port,
            &handle->config.reportClientConfig, &reportErr);
    SclBootstrap_destroyResult(winner);
    winner = NULL;

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
    if (reportErr != MMS_REPORT_CLIENT_OK) {
        MmsReportClient_destroy(reportClient);
        IedModel_release(iedModel);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_REPORT_CLIENT_START;
            outDetail->reportClientError = reportErr;
        }
        return ORCHESTRATION_ERR_REPORT_CLIENT_FAILED;
    }

    /* --- stage 5: goose_subscriber - fail-hard: a failure here tears down
     * the already-started report client (and ipc_dispatcher) too, leaving
     * the handle clean and re-runnable rather than half-started. Record
     * callback unconditionally wired to ipc_dispatcher, same as stage 4. */
    GooseSubscriberError gooseErr;
    GooseSubscriberHandle gooseHandle = GooseSubscription_create(iedModel, interfaceId,
            &handle->config.gooseSubscriberConfig, &gooseErr);
    if (!gooseHandle) {
        MmsReportClient_destroy(reportClient);
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
    if (gooseErr != GOOSE_SUBSCRIBER_OK) {
        GooseSubscription_destroy(gooseHandle);
        MmsReportClient_destroy(reportClient);
        IedModel_release(iedModel);
        IpcDispatcher_stop(handle->ipcDispatcher);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_GOOSE_SUBSCRIBER_START;
            outDetail->gooseSubscriberError = gooseErr;
        }
        return ORCHESTRATION_ERR_GOOSE_SUBSCRIBER_FAILED;
    }

    handle->iedModel = iedModel;
    handle->reportClient = reportClient;
    handle->gooseSubscriber = gooseHandle;
    handle->running = true;

    return ORCHESTRATION_OK;
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
