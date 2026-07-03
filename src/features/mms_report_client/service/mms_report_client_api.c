#include <stdlib.h>
#include "features/mms_report_client/service/mms_report_client_api.h"
#include "features/mms_report_client/data/mms_report_client_connection.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"

void
MmsReportClientConfig_defaults(MmsReportClientConfig* config) {
    if (!config) return;

    config->generalInterrogationOnEnable = true;
    config->connectTimeoutMs = 0;
    config->requestTimeoutMs = 0;
    config->reconnectInitialDelayMs = 1000;
    config->reconnectMaxDelayMs = 30000;
}

MmsReportClientHandle
MmsReportClient_create(IedModelHandle iedModel, const char* host, int port,
        const MmsReportClientConfig* config, MmsReportClientError* outError) {
    if (!iedModel || !host || port <= 0) {
        if (outError) *outError = MMS_REPORT_CLIENT_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    MmsReportClientHandle handle = calloc(1, sizeof(struct sMmsReportClientHandle));
    if (!handle) {
        if (outError) *outError = MMS_REPORT_CLIENT_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    handle->iedModel = iedModel;
    handle->host = MmsReportClientUtils_safeStringDup(host);
    handle->port = port;

    if (config) {
        handle->config = *config;
    } else {
        MmsReportClientConfig_defaults(&handle->config);
    }

    MmsReportClientError connError = MmsReportClientConnection_create(handle);
    if (connError != MMS_REPORT_CLIENT_OK) {
        free(handle->host);
        free(handle);
        if (outError) *outError = connError;
        return NULL;
    }

    if (outError) *outError = MMS_REPORT_CLIENT_OK;
    return handle;
}

void
MmsReportClient_setReportCallback(MmsReportClientHandle client,
        MmsReportClientCallback callback, void* userParam) {
    if (!client) return;
    client->reportCallback = callback;
    client->reportCallbackParam = userParam;
}

void
MmsReportClient_setConnectionStateCallback(MmsReportClientHandle client,
        MmsReportClientConnStateCallback callback, void* userParam) {
    if (!client) return;
    client->connStateCallback = callback;
    client->connStateCallbackParam = userParam;
}

void
MmsReportClient_setRcbStatusCallback(MmsReportClientHandle client,
        MmsReportClientRcbStatusCallback callback, void* userParam) {
    if (!client) return;
    client->rcbStatusCallback = callback;
    client->rcbStatusCallbackParam = userParam;
}

MmsReportClientError
MmsReportClient_start(MmsReportClientHandle client) {
    if (!client) return MMS_REPORT_CLIENT_ERR_INVALID_ARGUMENT;

    client->targets = IedModel_getReportSubscriptionTargets(client->iedModel);
    if (!client->targets || LinkedList_size(client->targets) == 0) {
        if (client->targets) {
            LinkedList_destroyDeep(client->targets, IedModel_destroyReportControlBlockTarget);
        }
        client->targets = NULL;
        return MMS_REPORT_CLIENT_ERR_INVALID_ARGUMENT;
    }

    return MmsReportClientConnection_start(client);
}

void
MmsReportClient_stop(MmsReportClientHandle client) {
    if (!client) return;
    MmsReportClientConnection_stop(client);
}

void
MmsReportClient_destroy(MmsReportClientHandle client) {
    if (!client) return;

    MmsReportClientConnection_stop(client);
    MmsReportClientConnection_destroy(client);

    if (client->targets) {
        LinkedList_destroyDeep(client->targets, IedModel_destroyReportControlBlockTarget);
    }
    free(client->host);
    free(client);
}

void
MmsReportClient_destroyReportRecord(MmsReportRecord* record) {
    MmsReportClientUseCases_freeReportRecord(record);
}
