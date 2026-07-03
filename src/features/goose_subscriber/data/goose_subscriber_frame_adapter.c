#include "hal_time.h"
#include "features/goose_subscriber/data/goose_subscriber_frame_adapter.h"
#include "features/goose_subscriber/domain/goose_subscriber_usecases.h"

static GooseSubscriberTargetEntry*
findTargetEntry(GooseSubscriberHandle handle, GooseSubscriber subscriber) {
    for (int i = 0; i < handle->targetCount; i++) {
        if (handle->targetEntries[i].rawSubscriber == subscriber) return &handle->targetEntries[i];
    }
    return NULL;
}

void
GooseSubscriberFrameAdapter_onGooseReceived(GooseSubscriber subscriber, void* parameter) {
    GooseSubscriberHandle handle = (GooseSubscriberHandle) parameter;
    if (!handle) return;

    /* A frame that fails to parse/sequence (isValid()==false) has nothing
     * safe to normalize - drop it here without touching liveness state.
     * test=true/needsCommission=true frames ARE still forwarded below (not
     * silently dropped) - per goose_subscriber.h's IMPORTANT notes those must
     * not be treated as live data by a standard-compliant receiver, but
     * that's the caller's policy decision (e.g. ipc_dispatcher surfacing
     * them as diagnostics), not this adapter's to make silently. */
    if (!GooseSubscriber_isValid(subscriber)) return;

    /* This frame is genuinely fresh (non-duplicate) per libiec61850's own
     * stNum/sqNum check inside parseGoosePayload - record it as the last
     * known-valid moment and fire a VALID transition here, synchronously,
     * rather than relying solely on the liveness thread's periodic poll to
     * separately re-observe the same truth. See
     * data/goose_subscriber_connection.c's top comment on the liveness
     * thread for why a poll alone is not sufficient: on loopback, the very
     * next delivery of this same message (a same-host duplicate-tap
     * artifact) will flip GooseSubscriber_isValid() back to false within
     * milliseconds, a window the poll can trivially miss even though the
     * feed is healthy. This adapter runs exactly when a fresh frame lands,
     * so it can't miss it. */
    GooseSubscriberTargetEntry* entry = findTargetEntry(handle, subscriber);
    if (entry) {
        Semaphore_wait(handle->targetStateLock);

        GooseSubscriberStatus status;
        bool transitioned = GooseSubscriberUseCases_detectStatusTransition(
                entry->lastKnownValid, true, &status);

        entry->lastKnownValid = true;
        entry->lastValidAtMs = Hal_getMonotonicTimeInMs();

        Semaphore_post(handle->targetStateLock);

        if (transitioned && handle->statusCallback) {
            handle->statusCallback(handle->statusCallbackParam, entry->target->objectReference,
                    status, GOOSE_PARSE_ERROR_NO_ERROR);
        }
    }

    if (!handle->recordCallback) return;

    MmsValue* dataSetValues = GooseSubscriber_getDataSetValues(subscriber);
    int entryCount = dataSetValues ? MmsValue_getArraySize(dataSetValues) : 0;

    uint8_t srcMac[6];
    uint8_t dstMac[6];
    GooseSubscriber_getSrcMac(subscriber, srcMac);
    GooseSubscriber_getDstMac(subscriber, dstMac);

    bool hasVlan = GooseSubscriber_isVlanSet(subscriber);

    const char* const* memberRefs = entry ? (const char* const*) entry->memberReferences : NULL;
    int memberRefCount = entry ? entry->memberCount : 0;

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            GooseSubscriber_getGoCbRef(subscriber),
            GooseSubscriber_getGoId(subscriber),
            GooseSubscriber_getDataSet(subscriber),
            GooseSubscriber_getStNum(subscriber),
            GooseSubscriber_getSqNum(subscriber),
            GooseSubscriber_getConfRev(subscriber),
            GooseSubscriber_isTest(subscriber),
            GooseSubscriber_needsCommission(subscriber),
            GooseSubscriber_getTimeAllowedToLive(subscriber),
            GooseSubscriber_getTimestamp(subscriber),
            hasVlan,
            hasVlan ? GooseSubscriber_getVlanId(subscriber) : 0,
            hasVlan ? GooseSubscriber_getVlanPrio(subscriber) : 0,
            GooseSubscriber_getAppId(subscriber),
            srcMac, dstMac,
            dataSetValues, memberRefs, memberRefCount, entryCount);

    /* Allocation failure building the record: nothing safe to deliver - drop
     * this message rather than risk the caller dereferencing a partial one. */
    if (record) handle->recordCallback(handle->recordCallbackParam, record);
}
