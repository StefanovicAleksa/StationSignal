#include <stdlib.h>
#include "features/goose_subscriber/service/goose_subscriber_api.h"
#include "features/goose_subscriber/data/goose_subscriber_connection.h"
#include "features/goose_subscriber/domain/goose_subscriber_usecases.h"
#include "features/goose_subscriber/utils/goose_subscriber_utils.h"

static void
freeTargetEntries(GooseSubscriberHandle handle) {
    if (!handle->targetEntries) return;

    for (int i = 0; i < handle->targetCount; i++) {
        IedModel_destroyGooseSubscriptionTarget(handle->targetEntries[i].target);
    }
    free(handle->targetEntries);
    handle->targetEntries = NULL;
    handle->targetCount = 0;
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
    free(handle->interfaceId);
    free(handle);
}

void
GooseSubscription_destroyRecord(GooseSubscriberRecord* record) {
    GooseSubscriberUseCases_freeRecord(record);
}
