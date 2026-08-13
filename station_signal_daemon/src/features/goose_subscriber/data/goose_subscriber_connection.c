#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal_time.h"
#include "features/goose_subscriber/data/goose_subscriber_connection.h"
#include "features/goose_subscriber/data/goose_subscriber_frame_adapter.h"
#include "features/goose_subscriber/domain/goose_subscriber_usecases.h"
#include "log.h"

/* Sleeps in small chunks so GooseSubscriberConnection_stop()'s bounded wait
 * for the liveness thread to exit doesn't have to wait out a full poll
 * interval (hal_thread.h has no interruptible sleep primitive). */
static void
interruptibleSleep(GooseSubscriberHandle handle, uint32_t totalMs) {
    const uint32_t chunkMs = 20;
    uint32_t slept = 0;
    while (slept < totalMs && !handle->stopRequested) {
        uint32_t thisChunk = (totalMs - slept) < chunkMs ? (totalMs - slept) : chunkMs;
        Thread_sleep((int) thisChunk);
        slept += thisChunk;
    }
}

/* TimeAllowedToLive is only known once a GOOSE message has actually been
 * received for a target (SCL carries no TAL) - GooseSubscriber_getTimeAllowedToLive
 * returns 0 for a target that hasn't received anything yet, which doubles as
 * our "unknown" sentinel (a publisher declaring TAL=0 would be nonsensical). */
static int32_t
computeMinObservedTalMs(GooseSubscriberHandle handle) {
    int32_t minTal = -1;
    for (int i = 0; i < handle->targetCount; i++) {
        uint32_t tal = GooseSubscriber_getTimeAllowedToLive(handle->targetEntries[i].rawSubscriber);
        if (tal == 0) continue;
        if (minTal < 0 || (int32_t) tal < minTal) minTal = (int32_t) tal;
    }
    return minTal;
}

/*
 * Checks, at a low rate, whether each target has gone stale - GOOSE
 * reception itself stays 100% event-driven via GooseListener/
 * GooseReceiver_start's internal thread, which is what actually delivers
 * data AND drives VALID transitions directly (see
 * goose_subscriber_frame_adapter.c). This poll never gates or delays a
 * GooseSubscriberRecord delivery; it only detects the *absence* of frames,
 * which is structurally unobservable any other way in a connectionless
 * protocol (there is no "disconnect" event to push, unlike IedConnection's
 * state-changed handler). Approved narrow exception to CLAUDE.md's "no
 * cyclic polling" rule - see the plan doc.
 *
 * Deliberately does NOT re-poll GooseSubscriber_isValid() to detect
 * staleness (an earlier version of this thread did). On loopback, a raw
 * AF_PACKET/ETH_P_ALL socket sees each transmitted frame twice - its own
 * PACKET_OUTGOING tap plus the real PACKET_HOST receipt (same-host-only
 * artifact; a remote subscriber on real hardware would never see this).
 * libiec61850's own retransmission/duplicate check
 * (goose_receiver.c: stNum==stNum && sqNum>=sqNum) flips isValid() back to
 * false within the same frame's duplicate delivery, milliseconds after the
 * first delivery set it true - a window this thread's poll can trivially,
 * consistently miss, even though the feed is perfectly healthy. Instead,
 * staleness here means "no frame the frame adapter confirmed valid within
 * its own declared TimeAllowedToLive" - the same math GooseSubscriber_isValid()
 * uses internally, but driven by our own lastValidAtMs timestamp (set only
 * on genuinely fresh, non-duplicate frames) rather than the library's
 * duplicate-sensitive stateValid flag.
 */
static void*
livenessLoop(void* parameter) {
    GooseSubscriberHandle handle = (GooseSubscriberHandle) parameter;

    while (!handle->stopRequested) {
        uint64_t nowMs = Hal_getMonotonicTimeInMs();

        for (int i = 0; i < handle->targetCount; i++) {
            GooseSubscriberTargetEntry* entry = &handle->targetEntries[i];

            Semaphore_wait(handle->targetStateLock);

            bool wasValid = entry->lastKnownValid;
            bool hasEverBeenValid = entry->lastValidAtMs != 0;
            uint32_t tal = GooseSubscriber_getTimeAllowedToLive(entry->rawSubscriber);
            bool isStale = wasValid && hasEverBeenValid && tal > 0 &&
                    (nowMs - entry->lastValidAtMs) > tal;

            if (isStale) {
                entry->lastKnownValid = false;
            }

            Semaphore_post(handle->targetStateLock);

            if (isStale) {
                GooseParseError parseError = GooseSubscriber_getParseError(entry->rawSubscriber);
                GooseSubscriberStatus status = (parseError == GOOSE_PARSE_ERROR_NO_ERROR)
                        ? GOOSE_SUBSCRIBER_STATUS_STALE
                        : GOOSE_SUBSCRIBER_STATUS_INVALID_STATE;

                if (handle->statusCallback) {
                    handle->statusCallback(handle->statusCallbackParam, entry->target->objectReference,
                            status, parseError);
                }
            }
        }

        handle->effectivePollMs = GooseSubscriberUseCases_computeLivenessPollIntervalMs(
                handle->config.livenessPollMs, computeMinObservedTalMs(handle));
        interruptibleSleep(handle, handle->effectivePollMs);
    }

    handle->livenessExited = true;
    return NULL;
}

GooseSubscriberError
GooseSubscriberConnection_create(GooseSubscriberHandle handle) {
    handle->receiver = GooseReceiver_create();
    if (!handle->receiver) return GOOSE_SUBSCRIBER_ERR_OUT_OF_MEMORY;

    handle->targetStateLock = Semaphore_create(1);
    if (!handle->targetStateLock) {
        GooseReceiver_destroy(handle->receiver);
        handle->receiver = NULL;
        return GOOSE_SUBSCRIBER_ERR_OUT_OF_MEMORY;
    }

    GooseReceiver_setInterfaceId(handle->receiver, handle->interfaceId);

    /* TEMPORARY diagnostic - remove once GOOSE reception silence is
     * root-caused (see the per-target [GOOSE_DIAG] line below). Confirms
     * this device actually built a non-empty GOOSE target list at all -
     * GooseSubscription_start already hard-fails with
     * GOOSE_SUBSCRIBER_ERR_NO_TARGETS if it didn't, but that failure aborts
     * the whole device start (torn down by orchestration's fail-hard
     * policy) and is easy to miss among unrelated MMS log volume. */
    SS_LOG_GOOSE("[GOOSE_DIAG] subscribing to %d GOOSE target(s) on interface '%s'\n",
            handle->targetCount, handle->interfaceId);

    for (int i = 0; i < handle->targetCount; i++) {
        GooseSubscriberTargetEntry* entry = &handle->targetEntries[i];

        /* GooseSubscriber_create's goCbRef parameter is non-const char*, but
         * libiec61850 copies it internally - safe to pass our owned string
         * directly (same convention already relied on elsewhere in this
         * codebase for similarly-shaped third-party calls). */
        GooseSubscriber sub = GooseSubscriber_create(entry->target->objectReference, NULL);
        if (!sub) return GOOSE_SUBSCRIBER_ERR_OUT_OF_MEMORY;

        if (entry->target->hasAddress) {
            uint8_t dstMac[6];
            memcpy(dstMac, entry->target->dstMac, 6);
            GooseSubscriber_setDstMac(sub, dstMac);
            GooseSubscriber_setAppId(sub, entry->target->appId);
        }

        /* TEMPORARY diagnostic - remove once GOOSE reception silence is
         * root-caused. Prints exactly the filter this repo applies per
         * target, straight from parsed SCL, for direct comparison against a
         * tshark capture of the live publisher's actual frames. Moved from
         * SS_LOG_DEBUG to SS_LOG_GOOSE so every line of this investigation
         * (setup-time filter + runtime reception, below) lands in the one
         * dedicated file instead of split across stderr and here. */
        SS_LOG_GOOSE("[GOOSE_DIAG] target=%s hasAddress=%d dstMac=%02X-%02X-%02X-%02X-%02X-%02X appId=0x%04X vlanId=0x%04X\n",
                entry->target->objectReference, entry->target->hasAddress,
                entry->target->dstMac[0], entry->target->dstMac[1], entry->target->dstMac[2],
                entry->target->dstMac[3], entry->target->dstMac[4], entry->target->dstMac[5],
                entry->target->appId, entry->target->vlanId);

        GooseSubscriber_setListener(sub, GooseSubscriberFrameAdapter_onGooseReceived, handle);

        entry->rawSubscriber = sub;
        entry->lastKnownValid = false;
        entry->lastValidAtMs = 0;
        entry->hasForwardedStNum = false;
        entry->lastForwardedStNum = 0;

        GooseReceiver_addSubscriber(handle->receiver, sub); /* must precede start(), per its doc note */
    }

    return GOOSE_SUBSCRIBER_OK;
}

GooseSubscriberError
GooseSubscriberConnection_start(GooseSubscriberHandle handle) {
    GooseReceiver_start(handle->receiver);
    if (!GooseReceiver_isRunning(handle->receiver)) {
        /* TEMPORARY diagnostic - remove once GOOSE reception silence is
         * root-caused. A failed raw-socket start (missing CAP_NET_RAW, bad
         * interfaceId) is already an error return the caller must handle -
         * this just makes it visible in the same dedicated file as the rest
         * of this investigation instead of only as a return code. */
        SS_LOG_GOOSE("[GOOSE_DIAG] GooseReceiver_start on interface '%s' did NOT enter the "
                "running state - no frame will ever be delivered on this connection\n",
                handle->interfaceId);
        return GOOSE_SUBSCRIBER_ERR_RECEIVER_START_FAILED;
    }

    /* TEMPORARY diagnostic - remove once GOOSE reception silence is
     * root-caused. Confirms the raw socket genuinely entered the running
     * state - if this line is present and no per-frame [GOOSE_DIAG] line
     * from goose_subscriber_frame_adapter.c ever follows it, reception is
     * failing below libiec61850's own receiver thread (wrong interface,
     * VLAN offload stripping the tag, a filter mismatch versus what's
     * actually on the wire) rather than in this codebase's own filtering. */
    SS_LOG_GOOSE("[GOOSE_DIAG] GooseReceiver_start on interface '%s' entered the running state "
            "(%d target(s) registered)\n", handle->interfaceId, handle->targetCount);

    handle->stopRequested = false;
    handle->livenessExited = false;
    handle->effectivePollMs = GooseSubscriberUseCases_computeLivenessPollIntervalMs(
            handle->config.livenessPollMs, computeMinObservedTalMs(handle));

    handle->livenessThread = Thread_create(livenessLoop, handle, false);
    if (!handle->livenessThread) {
        GooseReceiver_stop(handle->receiver);
        return GOOSE_SUBSCRIBER_ERR_THREAD_CREATE_FAILED;
    }
    Thread_start(handle->livenessThread);

    return GOOSE_SUBSCRIBER_OK;
}

void
GooseSubscriberConnection_stop(GooseSubscriberHandle handle) {
    if (!handle || handle->stopRequested) return;

    handle->stopRequested = true;

    if (handle->livenessThread) {
        while (!handle->livenessExited) {
            Thread_sleep(20);
        }
    }

    if (handle->receiver) GooseReceiver_stop(handle->receiver);
}

void
GooseSubscriberConnection_destroy(GooseSubscriberHandle handle) {
    if (!handle) return;

    if (handle->livenessThread) {
        Thread_destroy(handle->livenessThread);
        handle->livenessThread = NULL;
    }
    if (handle->receiver) {
        GooseReceiver_destroy(handle->receiver); /* cascades: destroys every attached GooseSubscriber */
        handle->receiver = NULL;
    }
    if (handle->targetStateLock) {
        Semaphore_destroy(handle->targetStateLock);
        handle->targetStateLock = NULL;
    }
}
