#include <stdlib.h>
#include <string.h>
#include "orchestration/service/orchestration_api.h"
#include "orchestration/domain/orchestration_usecases.h"
#include "orchestration/data/orchestration_staging.h"

void
OrchestrationConfig_defaults(OrchestrationConfig* config) {
    if (!config) return;

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

    if (outError) *outError = ORCHESTRATION_OK;
    return handle;
}

void
Orchestration_setReportCallback(OrchestrationHandle handle,
        MmsReportClientCallback callback, void* userParam) {
    if (!handle) return;
    handle->reportCallback = callback;
    handle->reportCallbackParam = userParam;
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
Orchestration_setGooseRecordCallback(OrchestrationHandle handle,
        GooseSubscriberCallback callback, void* userParam) {
    if (!handle) return;
    handle->gooseRecordCallback = callback;
    handle->gooseRecordCallbackParam = userParam;
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

    /* --- stage 1: scl_bootstrap - short-lived handle, not stored on
     * OrchestrationHandle since scanAndFetch is one-shot */
    SclBootstrapError bootstrapErr;
    SclBootstrapHandle sclHandle = SclBootstrap_create(&handle->config.bootstrapConfig, &bootstrapErr);
    if (!sclHandle) {
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
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_MODEL_LOAD;
            outDetail->modelLoadError = modelErr;
        }
        return ORCHESTRATION_ERR_MODEL_LOAD_FAILED;
    }

    /* --- stage 4: mms_report_client, against the winning candidate's own
     * host/port - MmsReportClient_create copies host internally, so winner
     * only needs to survive this one call */
    MmsReportClientError reportErr;
    MmsReportClientHandle reportClient = MmsReportClient_create(iedModel, winner->host, winner->port,
            &handle->config.reportClientConfig, &reportErr);
    SclBootstrap_destroyResult(winner);
    winner = NULL;

    if (!reportClient) {
        IedModel_release(iedModel);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_REPORT_CLIENT_START;
            outDetail->reportClientError = reportErr;
        }
        return ORCHESTRATION_ERR_REPORT_CLIENT_FAILED;
    }

    if (handle->reportCallback) {
        MmsReportClient_setReportCallback(reportClient, handle->reportCallback, handle->reportCallbackParam);
    }
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
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_REPORT_CLIENT_START;
            outDetail->reportClientError = reportErr;
        }
        return ORCHESTRATION_ERR_REPORT_CLIENT_FAILED;
    }

    /* --- stage 5: goose_subscriber - fail-hard: a failure here tears down
     * the already-started report client too, leaving the handle clean and
     * re-runnable rather than half-started */
    GooseSubscriberError gooseErr;
    GooseSubscriberHandle gooseHandle = GooseSubscription_create(iedModel, interfaceId,
            &handle->config.gooseSubscriberConfig, &gooseErr);
    if (!gooseHandle) {
        MmsReportClient_destroy(reportClient);
        IedModel_release(iedModel);
        if (outDetail) {
            outDetail->stage = ORCHESTRATION_STAGE_GOOSE_SUBSCRIBER_START;
            outDetail->gooseSubscriberError = gooseErr;
        }
        return ORCHESTRATION_ERR_GOOSE_SUBSCRIBER_FAILED;
    }

    if (handle->gooseRecordCallback) {
        GooseSubscription_setRecordCallback(gooseHandle, handle->gooseRecordCallback,
                handle->gooseRecordCallbackParam);
    }
    if (handle->gooseStatusCallback) {
        GooseSubscription_setStatusCallback(gooseHandle, handle->gooseStatusCallback,
                handle->gooseStatusCallbackParam);
    }

    gooseErr = GooseSubscription_start(gooseHandle);
    if (gooseErr != GOOSE_SUBSCRIBER_OK) {
        GooseSubscription_destroy(gooseHandle);
        MmsReportClient_destroy(reportClient);
        IedModel_release(iedModel);
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
    free(handle);
}
