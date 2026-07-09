#include <stdlib.h>
#include "features/goose_subscriber/service/goose_subscriber_api.h"
#include "features/goose_subscriber/data/goose_subscriber_connection.h"
#include "features/goose_subscriber/domain/goose_subscriber_usecases.h"
#include "features/goose_subscriber/utils/goose_subscriber_utils.h"

static void
freeTargetEntries(GooseSubscriberHandle handle) {
    if (!handle->targetEntries) return;

    for (int i = 0; i < handle->targetCount; i++) {
        GooseSubscriberMemberRefCache* cache = &handle->targetEntries[i].memberRefCache;

        IedModel_destroyGooseSubscriptionTarget(handle->targetEntries[i].target);
        for (int j = 0; j < cache->memberCount; j++) {
            free(cache->memberReferences[j]);
        }
        free(cache->memberReferences);

        if (cache->memberLeafReferences) {
            for (int j = 0; j < cache->memberCount; j++) {
                if (!cache->memberLeafReferences[j]) continue;
                for (int k = 0; k < cache->memberLeafCounts[j]; k++) {
                    free(cache->memberLeafReferences[j][k]);
                }
                free(cache->memberLeafReferences[j]);
            }
            free(cache->memberLeafReferences);
        }
        free(cache->memberLeafCounts);
        free(cache->leafSlotOffsets);

        if (cache->lastForwardedValues) {
            for (int k = 0; k < cache->totalLeafSlots; k++) {
                if (cache->lastForwardedValues[k]) MmsValue_delete(cache->lastForwardedValues[k]);
            }
            free(cache->lastForwardedValues);
        }
    }
    free(handle->targetEntries);
    handle->targetEntries = NULL;
    handle->targetCount = 0;
}

/*
 * Walks a LinkedList of heap-allocated char* strings into a freshly allocated
 * char** array, transferring ownership of each string into the array and
 * discarding just the list shell. Returns NULL and leaves *outCount at 0 if
 * the list is NULL/empty or allocation fails (the list, if any, is fully
 * destroyed either way). NULL-safe on `list` itself.
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
 * One-time, local resolution of a target's dataset member references (never
 * over-the-wire - see CLAUDE.md's "no over-the-wire tree discovery" rule).
 * GOOSE has no server-supplied reference alternative (unlike MMS's optional
 * DataRef), so this is always attempted, not just a fallback. Also resolves
 * the Gap 4 structure-decomposition metadata (memberLeafReferences/
 * memberLeafCounts) and the value-diff cache (leafSlotOffsets/totalLeafSlots/
 * lastForwardedValues, all-NULL/never-forwarded-yet) at the same time - see
 * GooseSubscriberMemberRefCache's own doc comment. Same slot-offset-
 * accumulation approach as mms_report_client_api.c's buildMemberRefCache.
 */
static void
resolveMemberReferences(GooseSubscriberTargetEntry* entry, IedModelHandle iedModel) {
    GooseSubscriberMemberRefCache* cache = &entry->memberRefCache;
    cache->memberReferences = NULL;
    cache->memberCount = 0;
    cache->memberLeafReferences = NULL;
    cache->memberLeafCounts = NULL;
    cache->leafSlotOffsets = NULL;
    cache->totalLeafSlots = 0;
    cache->lastForwardedValues = NULL;
    if (!entry->target->datasetReference) return;

    int count = 0;
    char** array = linkedListToStringArray(
            IedModel_getDataSetMemberReferences(iedModel, entry->target->datasetReference), &count);
    if (count == 0 || !array) return;

    char*** leafRefsArray = calloc((size_t) count, sizeof(char**));
    int* leafCounts = calloc((size_t) count, sizeof(int));
    int* leafOffsets = calloc((size_t) count, sizeof(int));
    int totalLeafSlots = 0;

    if (leafRefsArray && leafCounts && leafOffsets) {
        for (int m = 0; m < count; m++) {
            int leafCount = 0;
            char** leafArray = linkedListToStringArray(
                    IedModel_getDataSetMemberLeafReferences(iedModel, entry->target->datasetReference, m),
                    &leafCount);
            leafRefsArray[m] = leafArray; /* NULL if member m isn't decomposed */
            leafCounts[m] = leafCount;    /* 0 if member m isn't decomposed */

            /* Value-diff cache slot(s) for member m: leafCount consecutive
             * slots if decomposed, else exactly 1. */
            leafOffsets[m] = totalLeafSlots;
            totalLeafSlots += (leafCount > 0) ? leafCount : 1;
        }
    } else {
        free(leafRefsArray);
        free(leafCounts);
        free(leafOffsets);
        leafRefsArray = NULL;
        leafCounts = NULL;
        leafOffsets = NULL;
    }

    /* Zero-initialized so every slot starts NULL (never forwarded yet). */
    MmsValue** lastForwardedValues = (leafRefsArray && leafCounts && leafOffsets)
            ? calloc((size_t) totalLeafSlots, sizeof(MmsValue*)) : NULL;

    cache->memberReferences = array;
    cache->memberCount = count;
    cache->memberLeafReferences = leafRefsArray;
    cache->memberLeafCounts = leafCounts;
    cache->leafSlotOffsets = leafOffsets;
    cache->totalLeafSlots = totalLeafSlots;
    cache->lastForwardedValues = lastForwardedValues;
}

void
GooseSubscriberConfig_defaults(GooseSubscriberConfig* config) {
    if (!config) return;

    config->livenessPollMs = 0; /* auto-derive from observed TAL */
}

GooseSubscriberHandle
GooseSubscription_create(IedModelHandle iedModel, const char* interfaceId,
        const GooseSubscriberConfig* config, GooseSubscriberError* outError) {
    if (!iedModel || !interfaceId || interfaceId[0] == '\0') {
        if (outError) *outError = GOOSE_SUBSCRIBER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    GooseSubscriberHandle handle = calloc(1, sizeof(struct sGooseSubscriberHandle));
    if (!handle) {
        if (outError) *outError = GOOSE_SUBSCRIBER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    handle->iedModel = iedModel;
    handle->interfaceId = GooseSubscriberUtils_safeStringDup(interfaceId);

    if (config) {
        handle->config = *config;
    } else {
        GooseSubscriberConfig_defaults(&handle->config);
    }

    if (outError) *outError = GOOSE_SUBSCRIBER_OK;
    return handle;
}

void
GooseSubscription_setRecordCallback(GooseSubscriberHandle handle,
        GooseSubscriberCallback callback, void* userParam) {
    if (!handle) return;
    handle->recordCallback = callback;
    handle->recordCallbackParam = userParam;
}

void
GooseSubscription_setStatusCallback(GooseSubscriberHandle handle,
        GooseSubscriberStatusCallback callback, void* userParam) {
    if (!handle) return;
    handle->statusCallback = callback;
    handle->statusCallbackParam = userParam;
}

GooseSubscriberError
GooseSubscription_start(GooseSubscriberHandle handle) {
    if (!handle) return GOOSE_SUBSCRIBER_ERR_INVALID_ARGUMENT;
    if (handle->running) return GOOSE_SUBSCRIBER_OK; /* idempotent */

    LinkedList targets = IedModel_getGooseSubscriptionTargets(handle->iedModel);
    if (!targets || LinkedList_size(targets) == 0) {
        if (targets) LinkedList_destroyDeep(targets, IedModel_destroyGooseSubscriptionTarget);
        return GOOSE_SUBSCRIBER_ERR_NO_TARGETS;
    }

    int count = LinkedList_size(targets);
    GooseSubscriberTargetEntry* entries = calloc((size_t) count, sizeof(GooseSubscriberTargetEntry));
    if (!entries) {
        LinkedList_destroyDeep(targets, IedModel_destroyGooseSubscriptionTarget);
        return GOOSE_SUBSCRIBER_ERR_OUT_OF_MEMORY;
    }

    /* Move ownership of each GooseSubscriptionTarget* out of the LinkedList
     * into a flat array (needed for O(1) indexed iteration by the liveness
     * thread), then discard the list shell with LinkedList_destroyStatic -
     * NOT LinkedList_destroy, which (per linked_list.h) frees every element's
     * data too and would double-free the targets we just took ownership of.
     * Different from mms_report_client, which keeps the whole cached
     * LinkedList as-is since it never needs indexed access. */
    int i = 0;
    LinkedList element = LinkedList_getNext(targets);
    while (element) {
        entries[i].target = (GooseSubscriptionTarget*) LinkedList_getData(element);
        entries[i].rawSubscriber = NULL;
        entries[i].lastKnownValid = false;
        resolveMemberReferences(&entries[i], handle->iedModel);
        i++;
        element = LinkedList_getNext(element);
    }
    LinkedList_destroyStatic(targets);

    handle->targetEntries = entries;
    handle->targetCount = count;

    GooseSubscriberError err = GooseSubscriberConnection_create(handle);
    if (err != GOOSE_SUBSCRIBER_OK) {
        GooseSubscriberConnection_destroy(handle);
        freeTargetEntries(handle);
        return err;
    }

    err = GooseSubscriberConnection_start(handle);
    if (err != GOOSE_SUBSCRIBER_OK) {
        GooseSubscriberConnection_destroy(handle);
        freeTargetEntries(handle);
        return err;
    }

    handle->running = true;
    return GOOSE_SUBSCRIBER_OK;
}

void
GooseSubscription_stop(GooseSubscriberHandle handle) {
    if (!handle) return;
    GooseSubscriberConnection_stop(handle);
    handle->running = false;
}

void
GooseSubscription_destroy(GooseSubscriberHandle handle) {
    if (!handle) return;

    GooseSubscriberConnection_stop(handle);
    GooseSubscriberConnection_destroy(handle);
    freeTargetEntries(handle);
    GooseSubscriberUseCases_destroyCrossTargetDedupCache(&handle->crossTargetDedupCache);
    free(handle->interfaceId);
    free(handle);
}

void
GooseSubscription_destroyRecord(GooseSubscriberRecord* record) {
    GooseSubscriberUseCases_freeRecord(record);
}
