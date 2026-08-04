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
    char* reference; /* owned copy, resolved from ied_model's SCL dataset -
                         ALWAYS attempted (unlike MMS, GOOSE has no
                         server-supplied alternative), NULL only if
                         resolution failed (e.g. dataset lookup miss). */

    /* Owned clone of whatever was cached for this exact wire position
     * immediately BEFORE this frame overwrote it (see
     * GooseSubscriberMemberRefCache.lastForwardedValues) - mirrors
     * MmsReportEntry.previousValue exactly, including the "populated once,
     * preserved forever, never reset on recovery" design - see
     * GooseSubscriberMemberRefCache's own doc comment. NULL means either this
     * target's genuine first-ever frame (expected, routine) or this position
     * has no cache slot at all (slot < 0). */
    MmsValue* previousValue;

    /* IED_MODEL_DA_SEMANTIC_DBPOS if this leaf's real SCL bType was
     * genuinely "Dbpos", else IED_MODEL_DA_SEMANTIC_NONE - mirrors
     * MmsReportEntry.semantic exactly. */
    IedModelDaSemantic semantic;
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
 * Per-target Gap-4 decomposition metadata plus the value-diff cache used by
 * the group-aware event filter (see goose_subscriber_usecases.c's buildEntries
 * doc comment) - mirrors mms_report_client's MmsReportClientMemberRefCacheEntry,
 * minus any ReasonForInclusion concept (GOOSE has none): every candidate is
 * diff-gated unconditionally, a NULL cache slot being the only thing that
 * unconditionally survives (the case that lets the first-frame-ever full
 * snapshot through - see lastForwardedValues's own field comment below for
 * why this is now ONLY the first frame ever, not every recovery). Extracted
 * as its own struct, rather than folding these fields directly into
 * GooseSubscriberTargetEntry's public shape, so goose_subscriber_usecases.c's
 * pure-logic functions stay constructible with plain data in unit tests,
 * with no GooseSubscriptionTarget/GooseSubscriber dependency - same
 * rationale as MmsReportClientMemberRefCacheEntry's own separation from
 * ReportControlBlockTarget.
 */
typedef struct {
    char** memberReferences; /* owned array of owned strings, resolved once at
                                 GooseSubscription_start via
                                 IedModel_getDataSetMemberReferences(target->datasetReference);
                                 index i matches GooseSubscriberEntry[i] */
    int memberCount;

    /* Gap 4 (structure decomposition): memberLeafReferences[i] is NULL if
     * raw member i is already leaf-level (nothing to decompose); otherwise
     * an owned array of memberLeafCounts[i] owned leaf-reference strings,
     * from IedModel_getDataSetMemberLeafReferences, resolved once here
     * alongside memberReferences. */
    char*** memberLeafReferences;
    int* memberLeafCounts;

    /* Parallel to memberLeafReferences (same shape) - each decomposed
     * member's own per-leaf EXPECTED DataAttributeType, from
     * IedModel_getDataSetMemberLeafWireTypes. Passed into
     * reorderFlattenedToMatchReferences (goose_subscriber_usecases.c) as a
     * disambiguation signal for the reorder - only used to resolve which
     * remaining wire value belongs to a leaf when its expected type uniquely
     * identifies exactly one remaining candidate; NEVER used to reject the
     * decomposition outright - see
     * MmsReportClientMemberRefCacheEntry.memberLeafWireTypes's own doc
     * comment (mms_report_client_types.h) for the real-hardware finding this
     * guards against (confirmed twice: q/stVal swapped on one device,
     * stVal/stSeld swapped on another); identical rationale applies to
     * GOOSE, which carries the same structured DA types. May be NULL
     * (degrades to "no type check, trust positional order") if allocation
     * failed at build time. */
    DataAttributeType** memberLeafWireTypes;

    /* Value-diff cache. One slot per *expanded leaf* position: a
     * non-decomposed member i occupies exactly 1 slot at leafSlotOffsets[i];
     * a decomposed member i occupies memberLeafCounts[i] consecutive slots
     * starting there. totalLeafSlots is the flattened slot count (allocated
     * size of lastForwardedValues). Each slot starts NULL (never forwarded
     * yet); mutated in place by GooseSubscriberUseCases_buildRecord. Needed
     * because Gap 2's stNum dedup only catches whole-frame heartbeat
     * retransmissions - a real stNum-advancing change on ANY dataset member
     * would otherwise still forward every member, including untouched
     * siblings (e.g. a Quality DA that didn't change alongside a value that
     * did), the same noise problem mms_report_client's hybrid filter solves
     * on the MMS side.
     *
     * **Populated exactly once, on this target's first-ever valid frame, and
     * PRESERVED for the rest of this subscriber's lifetime - NEVER reset on
     * a STALE/INVALID_STATE -> VALID recovery.** (This supersedes an earlier
     * design where the whole cache was wiped back to NULL on every recovery
     * via a since-deleted `GooseSubscriberUseCases_resetValueDiffCache`
     * function - that made every recovery look like a fresh bootstrap,
     * discarding the real last-known value and reporting
     * `previousValue: null` right when a real value existed a moment
     * before.) A recovery's own fresh full snapshot (the first frame after
     * GooseSubscriber_isValid() goes back to true) now diffs against the
     * REAL, preserved last-known values from before the outage instead: a
     * genuine change made while the target was stale/invalid is correctly
     * forwarded with a real previousValue, an unchanged resend is correctly
     * suppressed by the ordinary diff check - see shouldForwardAndUpdateCache
     * in goose_subscriber_usecases.c. Only Gap 2's hasForwardedStNum still
     * resets on that transition (an unrelated heartbeat-dedup concern - see
     * the frame adapter). */
    int* leafSlotOffsets;
    int totalLeafSlots;
    MmsValue** lastForwardedValues;

    /* Parallel to lastForwardedValues (size totalLeafSlots) - slot i's
     * Dbpos-ness, resolved once at GooseSubscription_start time via
     * IedModel_getDataSetMemberSemantics/_getDataSetMemberLeafSemantics.
     * Unlike lastForwardedValues, never reset on a liveness recovery (a DA's
     * real SCL type doesn't change). May be NULL (degrades to
     * IED_MODEL_DA_SEMANTIC_NONE everywhere) if allocation failed at build
     * time. */
    IedModelDaSemantic* leafSemantics;

    /* Set to true, once, at the end of the first frame this target ever
     * processes (see buildEntries in goose_subscriber_usecases.c) - purely
     * to gate the debug logging in shouldForwardAndUpdateCache: a NULL cache
     * slot found while this is still false is the expected, routine
     * first-ever-frame case (nothing to log); a NULL slot found after this
     * is true should be structurally impossible under the never-reset design
     * above and is logged loudly on every occurrence as a bug worth
     * investigating. Zero-initialized to false for free, since this struct
     * is embedded inside GooseSubscriberTargetEntry, whose whole array is
     * calloc'd in GooseSubscription_start (unlike mms_report_client's
     * equivalent field, which needs explicit initialization - that struct is
     * malloc'd, not calloc'd). */
    bool everPopulated;
} GooseSubscriberMemberRefCache;

/*
 * One cached target plus its live GooseSubscriber and last-known liveness
 * state, for O(1) indexed iteration by the liveness thread. Owns `target`
 * (moved from the LinkedList returned by IedModel_getGooseSubscriptionTargets
 * at GooseSubscription_start time). Does NOT own rawSubscriber's destruction -
 * GooseReceiver_destroy cascades to every attached GooseSubscriber.
 */
typedef struct {
    GooseSubscriptionTarget* target;
    GooseSubscriberMemberRefCache memberRefCache;
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

    /* Heartbeat dedup: a GOOSE publisher retransmits its last state
     * periodically per MinTime/MaxTime even with no real change (sqNum
     * increments, stNum stays the same) - GooseSubscriber_isValid() accepts
     * these retransmissions as fresh, non-duplicate frames, so without this
     * the frame adapter would forward a record on every heartbeat, not just
     * on genuine state changes. hasForwardedStNum=false means "nothing
     * forwarded yet for this target" (either truly first-ever, or reset
     * after a STALE/INVALID_STATE -> VALID transition so the next real frame
     * is always delivered at least once even if its stNum numerically
     * collides with a pre-stale value). Unlike lastKnownValid/lastValidAtMs,
     * these two fields are read/written ONLY by the frame adapter
     * (GooseReceiver's single reception thread, confirmed serial across all
     * targets) - the liveness thread never touches them, so they are
     * deliberately NOT guarded by targetStateLock. memberRefCache's own
     * lastForwardedValues is reset alongside hasForwardedStNum on the same
     * STALE/INVALID_STATE -> VALID transition, for the same reason - see
     * that struct's own doc comment. */
    bool hasForwardedStNum;
    uint32_t lastForwardedStNum;
} GooseSubscriberTargetEntry;

/*
 * One (reference, value) pair kept by GooseSubscriberRecentForwardCache -
 * mirrors mms_report_client's MmsReportClientDedupEntry exactly. Both owned.
 */
typedef struct {
    char* reference;
    MmsValue* value;
} GooseSubscriberDedupEntry;

/*
 * Cross-target duplicate-content suppression - the GOOSE equivalent of
 * mms_report_client's MmsReportClientCrossRcbDedupCache (see its own doc
 * comment for the full rationale: some real networks configure multiple
 * GoCBs across independent LNs that publish the exact same underlying
 * event, mirroring the MMS redundant-RCB pattern - each target's own
 * per-position filter starts independent, so both frames would otherwise
 * survive their own filter and reach the websocket as apparent duplicates).
 * A candidate is a duplicate - and suppressed - only if its (reference,
 * value) content exactly matches a slot in this history AND its goCbRef
 * differs from the one that produced that slot; a repeat from the SAME
 * goCbRef is left entirely to that target's own per-position filter, same
 * as MMS leaves same-RCB repeats to its own per-RCB filter. Deliberately
 * content-only, no timestamp involved (an earlier revision required an
 * exact match on GOOSE's `t` field too, on the theory that independent
 * GoCBs republishing "the same" event would carry the same `t` - real
 * hardware disproved that: two independent publishers stamp their own `t`
 * independently and can differ by more than 0ms even for the same
 * underlying change, so requiring an exact match silently let real
 * cross-GoCB duplicates back through).
 *
 * A single "last forwarded, from any target" slot (the original design,
 * mirroring MMS's) only catches this when nothing else is forwarded in
 * between - GOOSE, unlike MMS, typically has many GoCBs streaming
 * concurrently on one IED, so an unrelated target's frame landing between
 * two duplicates routinely clobbers the one slot before the real duplicate
 * arrives. This struct is instead a small bounded ring of the most
 * recently forwarded records (any target), so a duplicate is still caught
 * even with unrelated traffic interleaved.
 *
 * Not to be confused with GooseSubscriberMemberRefCache's per-target
 * value-diff cache, which only ever compares a frame against that SAME
 * target's own history and knows nothing about content across targets.
 */
typedef struct {
    char* goCbRef;                        /* owned; NULL means an empty slot */
    GooseSubscriberDedupEntry* entries;   /* owned array of entryCount owned entries */
    int entryCount;
} GooseSubscriberRecentForwardRecord;

/* Ring capacity: sized generously above the concurrent-GoCB counts observed
 * on real IEDs (single devices seen streaming 8-10+ concurrent LN datasets)
 * so an interleaved burst of unrelated GOOSE traffic doesn't evict the
 * record a later duplicate needs to match against. Correctness of the dedup
 * decision doesn't depend on this size - the required exact timestampMs
 * match is what prevents false-positive suppression, not ring depth. */
#define GOOSE_SUBSCRIBER_RECENT_FORWARD_CAPACITY 32

typedef struct {
    GooseSubscriberRecentForwardRecord history[GOOSE_SUBSCRIBER_RECENT_FORWARD_CAPACITY];
    int count;     /* valid slots filled so far, caps at capacity */
    int nextSlot;  /* ring write cursor - wraps and overwrites the oldest slot once full */
} GooseSubscriberRecentForwardCache;

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
    GooseSubscriberRecentForwardCache recentForwardCache; /* zero-initialized by
                                   calloc in GooseSubscription_create (count == 0 means
                                   "nothing forwarded yet") - see its own doc comment above */

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
