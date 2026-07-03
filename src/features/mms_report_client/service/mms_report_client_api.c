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

/*
 * One-time, local resolution of each target's dataset member references
 * (never over-the-wire - see CLAUDE.md's "no over-the-wire tree discovery"
 * rule), used as a fallback for MmsReportEntry.reference when the server's
 * RCB doesn't have DataRef in its OptFlds. Built once at start; never rebuilt
 * on reconnect (same lifetime as client->targets).
 */
static LinkedList
buildMemberRefCache(MmsReportClientHandle client) {
    LinkedList cache = LinkedList_create();
    if (!cache) return NULL;

    LinkedList element = LinkedList_getNext(client->targets);
    while (element) {
        ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);
        if (target->datasetReference) {
            LinkedList refs = IedModel_getDataSetMemberReferences(client->iedModel, target->datasetReference);
            int count = refs ? LinkedList_size(refs) : 0;
            if (count > 0) {
                char** array = calloc((size_t) count, sizeof(char*));
                if (array) {
                    int i = 0;
                    LinkedList refElement = LinkedList_getNext(refs);
                    while (refElement) {
                        array[i++] = (char*) LinkedList_getData(refElement);
                        refElement = LinkedList_getNext(refElement);
                    }
                    /* ownership of each string transferred into array[];
                     * LinkedList_destroyStatic (NOT _destroy/_destroyDeep)
                     * discards only the list shell - same pattern
                     * goose_subscriber_api.c already uses for its own targets list. */
                    LinkedList_destroyStatic(refs);

                    MmsReportClientMemberRefCacheEntry* cacheEntry =
                            malloc(sizeof(MmsReportClientMemberRefCacheEntry));
                    if (cacheEntry) {
                        cacheEntry->rcbReference = MmsReportClientUtils_safeStringDup(target->objectReference);
                        cacheEntry->memberReferences = array;
                        cacheEntry->memberCount = count;
                        LinkedList_add(cache, cacheEntry);
                    } else {
                        for (int j = 0; j < count; j++) free(array[j]);
                        free(array);
                    }
                } else {
                    LinkedList_destroyDeep(refs, free);
                }
            } else if (refs) {
                LinkedList_destroyDeep(refs, free);
            }
        }
        element = LinkedList_getNext(element);
    }
    return cache;
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

    client->memberRefCache = buildMemberRefCache(client);

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
    if (client->memberRefCache) {
        LinkedList_destroyDeep(client->memberRefCache, MmsReportClientUseCases_destroyMemberRefCacheEntry);
    }
    free(client->host);
    free(client);
}

void
MmsReportClient_destroyReportRecord(MmsReportRecord* record) {
    MmsReportClientUseCases_freeReportRecord(record);
}
