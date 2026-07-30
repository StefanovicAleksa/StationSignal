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

/* Same shape as linkedListToStringArray, for a LinkedList of heap-boxed
 * DataAttributeType* (see IedModel_getDataSetMemberLeafWireTypes) - copies
 * each boxed enum BY VALUE into the array and frees the boxed items. Returns
 * NULL and leaves *outCount at 0 on empty/NULL input or allocation failure
 * (list fully destroyed either way). */
static DataAttributeType*
linkedListToWireTypeArray(LinkedList list, int* outCount) {
    *outCount = 0;
    int count = list ? LinkedList_size(list) : 0;
    if (count <= 0) {
        if (list) LinkedList_destroyDeep(list, free);
        return NULL;
    }

    DataAttributeType* array = calloc((size_t) count, sizeof(DataAttributeType));
    if (!array) {
        LinkedList_destroyDeep(list, free);
        return NULL;
    }

    int i = 0;
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        DataAttributeType* boxed = (DataAttributeType*) LinkedList_getData(element);
        array[i++] = *boxed;
        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(list, free);

    *outCount = count;
    return array;
}

/* Same shape as linkedListToStringArray, for a LinkedList of heap-boxed
 * IedModelDaSemantic* (see IedModel_getDataSetMemberSemantics/
 * _getDataSetMemberLeafSemantics) - copies each boxed enum BY VALUE into the
 * array and frees the boxed items (unlike linkedListToStringArray, which
 * transfers string ownership - an enum has nothing to transfer). Returns NULL
 * and leaves *outCount at 0 on empty/NULL input or allocation failure (list
 * fully destroyed either way). */
static IedModelDaSemantic*
linkedListToSemanticArray(LinkedList list, int* outCount) {
    *outCount = 0;
    int count = list ? LinkedList_size(list) : 0;
    if (count <= 0) {
        if (list) LinkedList_destroyDeep(list, free);
        return NULL;
    }

    IedModelDaSemantic* array = calloc((size_t) count, sizeof(IedModelDaSemantic));
    if (!array) {
        LinkedList_destroyDeep(list, free);
        return NULL;
    }

    int i = 0;
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        IedModelDaSemantic* boxed = (IedModelDaSemantic*) LinkedList_getData(element);
        array[i++] = *boxed;
        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(list, free);

    *outCount = count;
    return array;
}

/*
 * One-time, local resolution of each target's dataset member references
 * (never over-the-wire - see CLAUDE.md's "no over-the-wire tree discovery"
 * rule), used as a fallback for MmsReportEntry.reference when the server's
 * RCB doesn't have DataRef in its OptFlds. Also builds the Gap 4 (structure
 * decomposition) and value-diff (hybrid event filter) per-RCB caches at the
 * same time - see MmsReportClientMemberRefCacheEntry's own doc comment. Built
 * once at start; never rebuilt on reconnect (same lifetime as client->targets)
 * - and each entry's lastForwardedValues slots are likewise populated once
 * and PRESERVED for the client's whole lifetime, never reset on a
 * (re-)enable, so a reconnect's GI snapshot diffs against the real,
 * preserved last-known values from before the disconnect instead of a
 * wiped-clean cache.
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
             * doc comment - "all the variables" for that LN). The
             * IedModel_getDataSetMemberLeafReferences call below naturally
             * no-ops for these entries (target->datasetReference is NULL, so
             * it returns an empty list for every member index) - correct,
             * since every member here is already a terminal leaf DA by
             * construction, nothing to decompose. This is the exact same
             * member list mms_report_client_connection.c's enableOneTarget
             * uses to actually create the dataset on connect - computed once
             * here, purely locally, so both sides stay in sync. */
            array = linkedListToStringArray(
                    IedModel_getReportableAttributeReferencesForLogicalNode(client->iedModel, target->lnReference),
                    &count);
        }

        if (count > 0 && array) {
            char*** leafRefsArray = calloc((size_t) count, sizeof(char**));
            int* leafCounts = calloc((size_t) count, sizeof(int));
            int* leafOffsets = calloc((size_t) count, sizeof(int));
            int totalLeafSlots = 0;

            /* Parallel to leafRefsArray/leafCounts (same shape, same
             * per-member walk) - each decomposed member's own per-leaf
             * EXPECTED DataAttributeType, cross-checked in collectCandidates
             * against the actual wire-decoded MmsType before a decomposition
             * zip is trusted. See MmsReportClientMemberRefCacheEntry.memberLeafWireTypes's
             * own doc comment for the real-hardware finding this guards against. */
            DataAttributeType** leafWireTypesArray = calloc((size_t) count, sizeof(DataAttributeType*));

            /* Per-raw-member Dbpos semantics (count-aligned with `array`
             * itself) plus, for decomposed members, per-leaf semantics
             * (mirrors leafRefsArray/leafCounts' own shape exactly, since
             * IedModel_getDataSetMemberLeafSemantics is the semantics
             * counterpart of IedModel_getDataSetMemberLeafReferences and
             * walks the identical tree in the identical order). Both are
             * NULL for the "Dyn" (no datasetReference) case - a known,
             * accepted v1 gap, see IedModel's own doc comment on
             * getDataSetMemberSemantics. */
            int rawSemCount = 0;
            IedModelDaSemantic* rawSemantics = target->datasetReference
                    ? linkedListToSemanticArray(
                            IedModel_getDataSetMemberSemantics(client->iedModel, target->datasetReference),
                            &rawSemCount)
                    : NULL;
            IedModelDaSemantic** leafSemArray = calloc((size_t) count, sizeof(IedModelDaSemantic*));
            int* leafSemCounts = calloc((size_t) count, sizeof(int));

            if (leafRefsArray && leafCounts && leafOffsets) {
                for (int m = 0; m < count; m++) {
                    int leafCount = 0;
                    char** leafArray = linkedListToStringArray(
                            IedModel_getDataSetMemberLeafReferences(client->iedModel,
                                    target->datasetReference, m),
                            &leafCount);

                    leafRefsArray[m] = leafArray; /* NULL if member m isn't decomposed */
                    leafCounts[m] = leafCount;    /* 0 if member m isn't decomposed */

                    if (leafWireTypesArray && target->datasetReference) {
                        int leafWireTypeCount = 0;
                        DataAttributeType* wireTypes = linkedListToWireTypeArray(
                                IedModel_getDataSetMemberLeafWireTypes(client->iedModel,
                                        target->datasetReference, m),
                                &leafWireTypeCount);
                        /* Only trust this member's wire-types array if its count
                         * matches leafCount - a mismatch here means the two
                         * IedModel walks somehow disagreed on leaf count for the
                         * SAME (datasetReference, m), which should be
                         * structurally impossible (identical traversal), but
                         * degrading to "no type check for this member" is
                         * strictly safer than indexing past the end of a
                         * shorter array. */
                        leafWireTypesArray[m] = (wireTypes && leafWireTypeCount == leafCount) ? wireTypes : NULL;
                        if (wireTypes && leafWireTypeCount != leafCount) free(wireTypes);
                    }

                    if (leafSemArray && leafSemCounts && target->datasetReference) {
                        int leafSemCount = 0;
                        leafSemArray[m] = linkedListToSemanticArray(
                                IedModel_getDataSetMemberLeafSemantics(client->iedModel,
                                        target->datasetReference, m),
                                &leafSemCount);
                        leafSemCounts[m] = leafSemCount;
                    }

                    /* Value-diff cache slot(s) for member m: leafCount
                     * consecutive slots if decomposed, else exactly 1. */
                    leafOffsets[m] = totalLeafSlots;
                    totalLeafSlots += (leafCount > 0) ? leafCount : 1;
                }
            }

            /* Zero-initialized so every slot starts NULL (never forwarded
             * yet) - see MmsReportClientMemberRefCacheEntry's own doc comment. */
            MmsValue** lastForwardedValues = (leafRefsArray && leafCounts && leafOffsets)
                    ? calloc((size_t) totalLeafSlots, sizeof(MmsValue*)) : NULL;

            /* Zero-initialized (IED_MODEL_DA_SEMANTIC_NONE == 0) so any
             * member/leaf whose semantic couldn't be resolved degrades
             * safely rather than crashing - flattened from
             * rawSemantics/leafSemArray below. Deliberately NOT part of this
             * cacheEntry's own success/failure gate (unlike
             * lastForwardedValues, which the hybrid filter itself depends
             * on) - a NULL leafSemantics just means no Dbpos labels for this
             * RCB, not a broken cache. */
            IedModelDaSemantic* leafSemantics = (leafRefsArray && leafCounts && leafOffsets)
                    ? calloc((size_t) totalLeafSlots, sizeof(IedModelDaSemantic)) : NULL;
            if (leafSemantics) {
                for (int m = 0; m < count; m++) {
                    if (leafCounts[m] > 0) {
                        if (leafSemArray && leafSemArray[m] && leafSemCounts[m] == leafCounts[m]) {
                            for (int k = 0; k < leafCounts[m]; k++) {
                                leafSemantics[leafOffsets[m] + k] = leafSemArray[m][k];
                            }
                        }
                    } else if (rawSemantics && m < rawSemCount) {
                        leafSemantics[leafOffsets[m]] = rawSemantics[m];
                    }
                }
            }

            /* Temporaries only used to build the flattened arrays above -
             * always freed here, regardless of the cacheEntry outcome below. */
            free(rawSemantics);
            if (leafSemArray) {
                for (int m = 0; m < count; m++) free(leafSemArray[m]);
                free(leafSemArray);
            }
            free(leafSemCounts);

            MmsReportClientMemberRefCacheEntry* cacheEntry = malloc(sizeof(MmsReportClientMemberRefCacheEntry));
            if (cacheEntry && leafRefsArray && leafCounts && leafOffsets && lastForwardedValues) {
                cacheEntry->rcbReference = MmsReportClientUtils_safeStringDup(target->objectReference);
                cacheEntry->memberReferences = array;
                cacheEntry->memberCount = count;
                cacheEntry->memberLeafReferences = leafRefsArray;
                cacheEntry->memberLeafCounts = leafCounts;
                cacheEntry->leafSlotOffsets = leafOffsets;
                cacheEntry->totalLeafSlots = totalLeafSlots;
                cacheEntry->lastForwardedValues = lastForwardedValues;
                cacheEntry->memberLeafWireTypes = leafWireTypesArray;
                cacheEntry->leafSemantics = leafSemantics;
                cacheEntry->everPopulated = false;
                cacheEntry->lastEntryId = NULL;
                LinkedList_add(cache, cacheEntry);
            } else {
                free(cacheEntry);
                for (int j = 0; j < count; j++) free(array[j]);
                free(array);
                if (leafRefsArray) {
                    for (int m = 0; m < count; m++) {
                        if (!leafRefsArray[m]) continue;
                        for (int k = 0; k < leafCounts[m]; k++) free(leafRefsArray[m][k]);
                        free(leafRefsArray[m]);
                    }
                    free(leafRefsArray);
                }
                free(leafCounts);
                free(leafOffsets);
                free(lastForwardedValues);
                if (leafWireTypesArray) {
                    for (int m = 0; m < count; m++) free(leafWireTypesArray[m]);
                    free(leafWireTypesArray);
                }
                free(leafSemantics);
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
