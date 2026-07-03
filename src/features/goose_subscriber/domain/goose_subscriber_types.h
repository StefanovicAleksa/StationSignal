#ifndef GOOSE_SUBSCRIBER_TYPES_H_
#define GOOSE_SUBSCRIBER_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "linked_list.h"
#include "mms_value.h"
#include "goose_receiver.h"
#include "goose_subscriber.h"
#include "hal_thread.h"
#include "features/ied_model/service/ied_model_api.h"

/*
 * Domain vocabulary for this feature IS libiec61850's GOOSE-subscriber
 * vocabulary (MmsValue, GooseSubscriber, GooseParseError) - same convention
 * as mms_report_client's domain layer using ClientReport/IedConnection
 * directly: this data genuinely is the feature's domain, not swappable
 * infrastructure.
 */

typedef enum {
    GOOSE_SUBSCRIBER_OK = 0,
    GOOSE_SUBSCRIBER_ERR_INVALID_ARGUMENT,
    GOOSE_SUBSCRIBER_ERR_OUT_OF_MEMORY,
    GOOSE_SUBSCRIBER_ERR_THREAD_CREATE_FAILED,
    GOOSE_SUBSCRIBER_ERR_NO_TARGETS,
    GOOSE_SUBSCRIBER_ERR_RECEIVER_START_FAILED /* GooseReceiver_start() left !isRunning() -
                                                    typically a raw-socket permission problem
                                                    (missing CAP_NET_RAW) or a bad interfaceId */
} GooseSubscriberError;

/*
 * GOOSE has no association/connection concept - unlike
 * MmsReportClientConnState (which reflects one client-wide MMS association),
 * this models per-target liveness, since each GoCB is an independent
 * connectionless multicast stream. Derived purely from GooseSubscriber_isValid()
 * + GooseSubscriber_getParseError() - see data/goose_subscriber_connection.c's
 * liveness thread.
 */
typedef enum {
    GOOSE_SUBSCRIBER_STATUS_VALID = 0,     /* isValid()==true: fresh, in-sequence */
    GOOSE_SUBSCRIBER_STATUS_STALE,         /* TimeAllowedToLive elapsed with no refresh */
    GOOSE_SUBSCRIBER_STATUS_INVALID_STATE  /* isValid()==false for a parse/seq reason,
                                               not simply TAL expiry - see lastParseError */
} GooseSubscriberStatus;

/* One data-set member's decoded value, deep-owned copy. */
typedef struct {
    MmsValue* value; /* owned deep copy (MmsValue_clone); NULL only if the
                         element itself was NULL in the source array */
} GooseSubscriberEntry;

/*
 * A single fully-decoded, fully-owned GOOSE state snapshot delivered to the
 * caller's callback. Caller owns it after the callback returns and must free
 * it with GooseSubscription_destroyRecord.
 */
typedef struct {
    char* goCbRef;     /* owned copy, e.g. "Breaker1CB1/LLN0$GO$gcbStatus" */
    char* goId;        /* owned copy, or NULL if the publisher omitted it */
    char* dataSet;     /* owned copy, or NULL */

    uint32_t stNum;
    uint32_t sqNum;
    uint32_t confRev;
    bool test;             /* IMPORTANT (goose_subscriber.h): standard-compliant
                               receivers must ignore test=true messages as live
                               data - this feature still forwards them (the
                               caller's policy decision), see frame adapter */
    bool needsCommission;  /* same IMPORTANT note applies to ndsCom=true */
    uint32_t timeAllowedToLiveMs;
    uint64_t timestampMs;  /* ms since 1970-01-01 UTC */

    bool hasVlan;
    uint16_t vlanId;   /* valid only if hasVlan */
    uint8_t vlanPrio;  /* valid only if hasVlan */
    int32_t appId;     /* -1 if not set, matches GooseSubscriber_getAppId's own convention */

    uint8_t srcMac[6];
    uint8_t dstMac[6];

    GooseSubscriberEntry* entries; /* owned array of entryCount elements */
    int entryCount;
} GooseSubscriberRecord;

typedef void (*GooseSubscriberCallback)(void* userParam, const GooseSubscriberRecord* record);

/*
 * Fires on a VALID<->non-VALID transition for one target, from the liveness
 * timer thread (see data/goose_subscriber_connection.c). Purely observational
 * - never required for record delivery, which stays fully event-driven via
 * GooseListener regardless of whether this callback is registered. Must not
 * block or call back into this feature's own API (same rule as
 * MmsReportClientConnStateCallback).
 */
typedef void (*GooseSubscriberStatusCallback)(void* userParam, const char* goCbRef,
        GooseSubscriberStatus status, GooseParseError lastParseError);

typedef struct {
    uint32_t livenessPollMs;   /* 0 = auto: derived from observed TimeAllowedToLive across
                                   targets, floored - see
                                   GooseSubscriberUseCases_computeLivenessPollIntervalMs */
} GooseSubscriberConfig;

/*
 * One cached target plus its live GooseSubscriber and last-known liveness
 * state, for O(1) indexed iteration by the liveness thread. Owns `target`
 * (moved from the LinkedList returned by IedModel_getGooseSubscriptionTargets
 * at GooseSubscription_start time). Does NOT own rawSubscriber's destruction -
 * GooseReceiver_destroy cascades to every attached GooseSubscriber.
 */
typedef struct {
    GooseSubscriptionTarget* target;
    GooseSubscriber rawSubscriber;
    bool lastKnownValid;
    uint64_t lastValidAtMs; /* Hal_getMonotonicTimeInMs() of the last frame the
                                frame adapter confirmed GooseSubscriber_isValid()
                                for; 0 = never. Set only from the frame
                                adapter (GooseReceiver's reception thread),
                                read+compared against by the liveness thread
                                under targetStateLock - see
                                data/goose_subscriber_connection.c's top
                                comment for why this, rather than re-polling
                                isValid() itself, is what the liveness thread
                                now checks for staleness. */
} GooseSubscriberTargetEntry;

/*
 * Internal representation. Defined here (rather than hidden behind an
 * additional internal-only header) because every file within this feature
 * needs field access, mirroring mms_report_client's struct
 * sMmsReportClientHandle convention: opacity is enforced by which header is
 * exposed (service/goose_subscriber_api.h is the only public one), not by
 * hiding the struct.
 */
struct sGooseSubscriberHandle {
    IedModelHandle iedModel; /* borrowed - caller retains ownership */
    char* interfaceId;       /* owned copy */
    GooseSubscriberConfig config;

    GooseReceiver receiver;                     /* owned */
    GooseSubscriberTargetEntry* targetEntries;  /* owned array */
    int targetCount;

    GooseSubscriberCallback recordCallback;
    void* recordCallbackParam;
    GooseSubscriberStatusCallback statusCallback;
    void* statusCallbackParam;

    /* liveness timer thread state - see USER DECISION 3 in the plan: GOOSE has
     * no connection-lost push signal, so staleness is detected by a narrow,
     * low-rate poll rather than an event. VALID transitions are now driven
     * directly by the frame adapter instead (event-driven, see
     * data/goose_subscriber_frame_adapter.c); this thread's remaining job is
     * purely to notice the *absence* of frames (staleness), which is the one
     * thing that structurally cannot be event-driven. */
    volatile bool stopRequested;
    volatile bool livenessExited;
    Thread livenessThread;
    uint32_t effectivePollMs;

    /* Guards targetEntries[i].lastKnownValid/lastValidAtMs and the
     * VALID<->non-VALID status callback firing, since both the frame adapter
     * (GooseReceiver's reception thread) and the liveness thread touch the
     * same per-target state concurrently. */
    Semaphore targetStateLock;

    volatile bool running;
};

typedef struct sGooseSubscriberHandle* GooseSubscriberHandle;

#endif /* GOOSE_SUBSCRIBER_TYPES_H_ */
