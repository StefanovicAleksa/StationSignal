#include <stdio.h>
#include "hal_time.h"
#include "features/goose_subscriber/data/goose_subscriber_frame_adapter.h"
#include "features/goose_subscriber/domain/goose_subscriber_usecases.h"
#include "log.h"

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

    /* TEMPORARY diagnostic - remove once GOOSE reception silence is
     * root-caused (see goose_subscriber_connection.c's own [GOOSE_DIAG]
     * lines). This is THE line that answers "does anything ever reach this
     * callback at all" - every other branch below this point was previously
     * silent on every path (valid-but-duplicate, valid-but-filtered-to-zero,
     * invalid/unparseable), so a dev-mode capture with GOOSE reception
     * apparently dead could not distinguish "no frames arrive at the raw
     * socket" from "frames arrive but get dropped somewhere in this
     * function." Fires unconditionally, before the validity check, since
     * GoCbRef/isValid/parseError are all safe to read regardless of parse
     * outcome. */
    SS_LOG_GOOSE("[GOOSE_DIAG] onGooseReceived: goCbRef=%s isValid=%d parseError=%d\n",
            GooseSubscriber_getGoCbRef(subscriber), GooseSubscriber_isValid(subscriber),
            (int) GooseSubscriber_getParseError(subscriber));

    /* A frame that fails to parse/sequence (isValid()==false) has nothing
     * safe to normalize - drop it here without touching liveness state.
     * test=true/needsCommission=true frames ARE still forwarded below (not
     * silently dropped) - per goose_subscriber.h's IMPORTANT notes those must
     * not be treated as live data by a standard-compliant receiver, but
     * that's the caller's policy decision (e.g. ipc_dispatcher surfacing
     * them as diagnostics), not this adapter's to make silently. */
    if (!GooseSubscriber_isValid(subscriber)) {
        SS_LOG_GOOSE("[GOOSE_DIAG] onGooseReceived: dropped - invalid/unparseable frame "
                "(goCbRef=%s)\n", GooseSubscriber_getGoCbRef(subscriber));
        return;
    }

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

        if (transitioned) {
            /* Re-validated (or first-ever) - forget whatever stNum was last
             * forwarded, so the next real frame's state is delivered at
             * least once even if its stNum numerically collides with a
             * pre-stale value (e.g. the publisher never actually restarted).
             * Safe without targetStateLock - only this thread ever touches
             * hasForwardedStNum/lastForwardedStNum. The per-position
             * value-diff cache (entry->memberRefCache) is deliberately NOT
             * touched here anymore - it's populated once, on this target's
             * very first-ever valid frame, and PRESERVED across every later
             * recovery (never reset) - see GooseSubscriberMemberRefCache's
             * own doc comment. The next frame's full snapshot diffs against
             * the REAL, preserved last-known values from before the outage
             * instead of against a wiped-clean cache. */
            entry->hasForwardedStNum = false;

            if (handle->statusCallback) {
                handle->statusCallback(handle->statusCallbackParam, entry->target->objectReference,
                        status, GOOSE_PARSE_ERROR_NO_ERROR);
            }
        }
    }

    if (!handle->recordCallback) return;

    uint32_t stNum = GooseSubscriber_getStNum(subscriber);
    if (entry && GooseSubscriberUseCases_isDuplicateStNum(
            entry->hasForwardedStNum, entry->lastForwardedStNum, stNum)) {
        /* Heartbeat retransmission (MinTime/MaxTime keep-alive) - the
         * dataset didn't change, nothing worth forwarding as an event. */
        SS_LOG_GOOSE("[GOOSE_DIAG] onGooseReceived: dropped - duplicate stNum=%u heartbeat "
                "(goCbRef=%s)\n", stNum, GooseSubscriber_getGoCbRef(subscriber));
        return;
    }

    MmsValue* dataSetValues = GooseSubscriber_getDataSetValues(subscriber);
    int entryCount = dataSetValues ? MmsValue_getArraySize(dataSetValues) : 0;

    uint8_t srcMac[6];
    uint8_t dstMac[6];
    GooseSubscriber_getSrcMac(subscriber, srcMac);
    GooseSubscriber_getDstMac(subscriber, dstMac);

    bool hasVlan = GooseSubscriber_isVlanSet(subscriber);

    GooseSubscriberMemberRefCache* memberRefCache = entry ? &entry->memberRefCache : NULL;

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            GooseSubscriber_getGoCbRef(subscriber),
            GooseSubscriber_getGoId(subscriber),
            GooseSubscriber_getDataSet(subscriber),
            stNum,
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
            dataSetValues, memberRefCache, entryCount);

    /* Allocation failure building the record: nothing safe to deliver - drop
     * this message rather than risk the caller dereferencing a partial one.
     * A record built with zero surviving entries (every candidate filtered
     * by the per-position value-diff cache - no observable change since the
     * last frame actually forwarded) is also dropped, not delivered - but
     * hasForwardedStNum/lastForwardedStNum still advance whenever a record
     * was built at all, so an identical-stNum heartbeat retransmission stays
     * caught by the cheap isDuplicateStNum gate above rather than re-running
     * this whole filter pipeline on every retransmission. */
    if (record) {
        if (entry) {
            entry->hasForwardedStNum = true;
            entry->lastForwardedStNum = stNum;
        }

        /* record->entryCount > 0: survived this target's own per-position
         * value-diff filter. Cross-GoCB duplicate-content suppression (a
         * different GoCB recently forwarding this exact same content) is no
         * longer this feature's own concern - ipc_dispatcher's own dedup
         * cache (IpcDispatcherUseCases_shouldForwardWithinProtocol) catches
         * it downstream instead, at the one point both this feature's GoCBs
         * AND mms_report_client's RCBs actually converge. */
        if (record->entryCount > 0) {
            SS_LOG_GOOSE("[GOOSE_DIAG] onGooseReceived: forwarding record - goCbRef=%s "
                    "stNum=%u sqNum=%u entryCount=%d\n", GooseSubscriber_getGoCbRef(subscriber),
                    stNum, GooseSubscriber_getSqNum(subscriber), record->entryCount);
            handle->recordCallback(handle->recordCallbackParam, record);
        } else {
            /* This is the bootstrap-seed / genuinely-unchanged case, not a
             * reception problem - but indistinguishable from one without
             * this line, since it's silent otherwise (mirrors
             * mms_report_client's "0 survived the per-RCB value-diff filter"
             * log, which GOOSE never had an equivalent of). */
            SS_LOG_GOOSE("[GOOSE_DIAG] onGooseReceived: dropped - 0 entries survived the "
                    "value-diff filter (bootstrap-seed or unchanged) - goCbRef=%s stNum=%u\n",
                    GooseSubscriber_getGoCbRef(subscriber), stNum);
            GooseSubscriberUseCases_freeRecord(record);
        }
    } else {
        SS_LOG_GOOSE("[GOOSE_DIAG] onGooseReceived: dropped - record build failed (allocation "
                "failure) - goCbRef=%s stNum=%u\n", GooseSubscriber_getGoCbRef(subscriber), stNum);
    }
}
