#include <stdlib.h>
#include "features/mms_report_client/service/mms_report_client_api.h"
#include "features/mms_report_client/data/mms_report_client_connection.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"

void
MmsReportClientConfig_defaults(MmsReportClientConfig* config) {
    if (!config) return;

    config->connectTimeoutMs = 0;
    config->requestTimeoutMs = 0;
    config->reconnectInitialDelayMs = 1000;
    config->reconnectMaxDelayMs = 30000;
    config->acseAuthPassword = NULL;
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

    /* Owned copy taken before MmsReportClientConnection_create (which is what
     * actually applies it to the IedConnection) - see MmsReportClientConfig's
     * own field doc comment for why this outlives the caller's buffer. */
    handle->ownedAuthPassword = MmsReportClientUtils_safeStringDup(handle->config.acseAuthPassword);
    handle->config.acseAuthPassword = handle->ownedAuthPassword;

    MmsReportClientError connError = MmsReportClientConnection_create(handle);
    if (connError != MMS_REPORT_CLIENT_OK) {
        free(handle->ownedAuthPassword);
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
 * Walks a LinkedList of heap-allocated char* strings into a freshly allocated
 * char** array, transferring ownership of each string into the array and
 * discarding just the list shell (LinkedList_destroyStatic, not _destroyDeep)
 * - same pattern goose_subscriber_api.c already uses for its own targets
 * list. Returns NULL and leaves *outCount at 0 if the list is NULL/empty or
 * allocation fails (the list, if any, is fully destroyed either way - never
 * leaked). NULL-safe on `list` itself.
 */
static char**
linkedListToStringArray(LinkedList list, int* outCount) {
    *outCount = 0;
    int count = list ? LinkedList_size(list) : 0;
    if (count <= 0) {
        if (list) LinkedList_destroyDeep(list, free);
        return NULL;
    }

    char** array = calloc((size_t) count, sizeof(char*));
    if (!array) {
        LinkedList_destroyDeep(list, free);
        return NULL;
    }

    int i = 0;
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        array[i++] = (char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);
    }
    LinkedList_destroyStatic(list);

    *outCount = count;
    return array;
}

/*
 * One-time, local resolution of each target's dataset member references
 * (never over-the-wire - see CLAUDE.md's "no over-the-wire tree discovery"
 * rule), used as a fallback for MmsReportEntry.reference when the server's
 * RCB doesn't have DataRef in its OptFlds. Also builds the Gap 4 (structure
 * decomposition) and value-diff (hybrid event filter) per-RCB caches at the
 * same time, via MmsReportClientUseCases_buildMemberRefCacheEntry - see
 * MmsReportClientMemberRefCacheEntry's own doc comment. Built once at start;
 * each entry's lastForwardedValues slots are populated once and PRESERVED
 * for the client's whole lifetime, never reset on a (re-)enable, so a
 * reconnect's GI snapshot diffs against the real, preserved last-known
 * values from before the disconnect instead of a wiped-clean cache - UNLESS
 * mms_report_client_connection.c's own MmsReportClientConnection_refreshPulledMemberRefCache/
 * _ensureLnFallbackMemberRefCache later detects (post-connect) that a
 * target's actual dataset shape has changed since this build, in which case
 * that one entry's own shape/value-diff-cache gets rebuilt then - see those
 * functions' own doc comments.
 */
static LinkedList
buildMemberRefCache(MmsReportClientHandle client) {
    LinkedList cache = LinkedList_create();
    if (!cache) return NULL;

    LinkedList element = LinkedList_getNext(client->targets);
    while (element) {
        ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);

        int count = 0;
        char** array;
        if (target->datasetReference) {
            array = linkedListToStringArray(
                    IedModel_getDataSetMemberReferences(client->iedModel, target->datasetReference), &count);
        } else {
            /* Dynamic RCB (SCL declared no datSet, datSet="Dyn") - fall back
             * to every FC=ST/MX leaf under the RCB's own LN (see
             * IedModel_getReportableAttributeReferencesForLogicalNode's own
             * doc comment - "all the variables" for that LN) as this entry's
             * PROVISIONAL shape. This is the exact same member list
             * getOrCreateDynamicDataset (mms_report_client_connection.c) uses
             * to actually create a dataset on connect if tier 3 (self-create)
             * ends up being used - but if tier 2 (pull a live-assigned
             * dataset) succeeds instead, this provisional shape gets replaced
             * post-connect before any report is decoded against it - see
             * MmsReportClientConnection_refreshPulledMemberRefCache's own doc
             * comment. */
            array = linkedListToStringArray(
                    IedModel_getReportableAttributeReferencesForLogicalNode(client->iedModel, target->lnReference),
                    &count);
        }

        if (count > 0 && array) {
            /* resolvedDatasetReference: tier 1 (static) is tagged with
             * target->datasetReference itself (immutable for this target's
             * whole lifetime - enableOneTarget never rechecks it, see
             * MmsReportClientMemberRefCacheEntry's own doc comment). Tier 3's
             * provisional LN-fallback shape built above is deliberately
             * tagged NULL - not yet known whether tier 2 (pull) or tier 3
             * (self-create) will actually be used at the first enable, so the
             * very first enableOneTarget call always treats this entry as
             * "needs resolving," cheaply confirmed (a no-op rebuild) the
             * moment either tier is decided. */
            MmsReportClientMemberRefCacheEntry* cacheEntry = MmsReportClientUseCases_buildMemberRefCacheEntry(
                    client->iedModel, target->objectReference, array, count, target->datasetReference);
            if (cacheEntry) LinkedList_add(cache, cacheEntry);
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
        return MMS_REPORT_CLIENT_ERR_NO_TARGETS;
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
    MmsReportClientUseCases_destroyCrossRcbDedupCache(&client->crossRcbDedupCache);
    free(client->host);
    free(client->ownedAuthPassword);
    free(client);
}

void
MmsReportClient_destroyReportRecord(MmsReportRecord* record) {
    MmsReportClientUseCases_freeReportRecord(record);
}
