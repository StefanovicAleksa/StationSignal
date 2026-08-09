#ifndef MMS_REPORT_CLIENT_TYPES_H_
#define MMS_REPORT_CLIENT_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "linked_list.h"
#include "iec61850_client.h"
#include "mms_value.h"
#include "hal_thread.h"
#include "features/ied_model/service/ied_model_api.h"
#include "features/mms_dataset_manager/service/mms_dataset_manager_api.h"

/*
 * Domain vocabulary for this feature IS libiec61850's MMS-client reporting
 * vocabulary (MmsValue, ReasonForInclusion, IedClientError, IedConnection) -
 * same convention as ied_model's domain layer using IedModel/FunctionalConstraint
 * directly: this data genuinely is the feature's domain, not swappable
 * infrastructure.
 */

typedef enum {
    MMS_REPORT_CLIENT_OK = 0,
    MMS_REPORT_CLIENT_ERR_INVALID_ARGUMENT,
    MMS_REPORT_CLIENT_ERR_OUT_OF_MEMORY,
    MMS_REPORT_CLIENT_ERR_THREAD_CREATE_FAILED,
    MMS_REPORT_CLIENT_ERR_NO_TARGETS /* SCL declared zero <ReportControl> blocks for this IED -
                                         mirrors GOOSE_SUBSCRIBER_ERR_NO_TARGETS */
} MmsReportClientError;

typedef enum {
    MMS_REPORT_CLIENT_DISCONNECTED = 0,
    MMS_REPORT_CLIENT_CONNECTING,
    MMS_REPORT_CLIENT_CONNECTED,
    MMS_REPORT_CLIENT_RECONNECT_BACKOFF,

    /* IedConnection_connect() did not reach CONNECTED - see supervisorLoop's own
     * comment (mms_report_client_connection.c) for exactly what this does and does
     * NOT mean: libiec61850 collapses every non-success connect outcome (a wrong
     * ACSE password, a plain TCP refusal/timeout, any other AARE-level reject) onto
     * the identical IED_ERROR_CONNECTION_REJECTED code, so this state is an honest
     * "didn't connect, for some rejection-shaped reason" signal, not a precise
     * "wrong password" diagnosis. Edge-triggered: fired once per rejection streak,
     * not once per backoff cycle - see connectionRejectedSignaled below. Retries
     * continue unconditionally regardless of this state; it is diagnostic-only. */
    MMS_REPORT_CLIENT_CONNECTION_REJECTED
} MmsReportClientConnState;

/*
 * One data-set member's value in a received report. reference/value are
 * owned, deep copies (MmsValue_clone'd / strdup'd) taken before the
 * originating ClientReport is invalidated - see data/mms_report_client_report_adapter.c.
 */
typedef struct {
    char* reference;      /* owned copy, or NULL if the server omitted data-references */
    MmsValue* value;      /* owned deep copy (MmsValue_clone) */
    ReasonForInclusion reason;

    /* Owned clone of whatever was cached for this exact wire position
     * immediately BEFORE this report overwrote it (see
     * MmsReportClientMemberRefCacheEntry.lastForwardedValues) - lets
     * ipc_dispatcher report what a changed point changed FROM, not just its
     * new value. The cache is populated exactly once, on this RCB's very
     * first-ever report after its first connect, and PRESERVED from then on
     * for the rest of the client's lifetime - never reset on reconnect (see
     * MmsReportClientMemberRefCacheEntry's own doc comment). So NULL here
     * means one of two things: either this genuinely is that RCB's first-ever
     * report (nothing to compare against yet - expected, routine), or this
     * position has no cache slot at all (memberRefCache == NULL or this
     * position isn't covered by it - see shouldForwardAndUpdateCache's own
     * slot < 0 branch in mms_report_client_usecases.c). Once a position has
     * been populated, previousValue should never be NULL again - see that
     * same function's everPopulated-gated debug logging for how an
     * unexpected NULL past that point gets surfaced. */
    MmsValue* previousValue;

    /* IED_MODEL_DA_SEMANTIC_DBPOS if this leaf's real SCL bType was
     * genuinely "Dbpos" (see IedModelDaSemantic's own doc comment in
     * ied_model_types.h), else IED_MODEL_DA_SEMANTIC_NONE - lets
     * ipc_dispatcher attach a descriptive label without guessing from the
     * wire type alone. */
    IedModelDaSemantic semantic;

    /* True if THIS leaf's own value differed from its cached last-forwarded
     * value in shouldForwardAndUpdateCache's phase-2a diff check, BEFORE
     * buildEntries' group-extension pass ran (see that function's own doc
     * comment) - i.e. this leaf individually qualified on its own merits,
     * as opposed to only being present because a DIFFERENT sibling DA under
     * the same DO/SDO anchor changed (e.g. a DPC's "t"/"stSeld" riding along
     * with an unrelated "stVal" change). Group membership/record->entries
     * itself is unaffected by this field - it exists purely so ipc_dispatcher
     * can tell the two cases apart when deciding what actually reaches the
     * outbound dataPoints array (see IpcDispatcherUseCases_shouldIncludeValuePoint). */
    bool ownChangeDetected;
} MmsReportEntry;

/*
 * Locally-resolved fallback for MmsReportEntry.reference, cached once per RCB
 * at MmsReportClient_start time from ied_model's SCL-derived dataset
 * (IedModel_getDataSetMemberReferences) - never re-resolved per report. Used
 * only when the server's report omits a data-reference for a given index.
 *
 * Also carries the Gap 4 (structure decomposition) cache built once at the
 * same time - see mms_report_client_report_adapter.c's onReport for how
 * these are used together.
 *
 * Event filtering ALWAYS diff-checks against the per-position
 * lastForwardedValues cache below, regardless of the server's own
 * ReasonForInclusion - forwarded only if its value differs from the last one
 * actually sent for that exact wire position.
 *
 * **The cache is populated exactly once, on first connect, and PRESERVED for
 * the rest of this client's lifetime - it is NEVER reset on reconnect.**
 * `enableOneTarget` (mms_report_client_connection.c) always requests GI on
 * every enable, first connect and every reconnect alike, purely to force an
 * immediate, deterministic snapshot to diff against - on first connect this
 * snapshot lands against the still-all-NULL cache and is silently
 * bootstrap-seeded (a NULL slot is the bootstrap case: the cache is seeded
 * from it, but it is NEVER forwarded itself - see shouldForwardAndUpdateCache
 * in mms_report_client_usecases.c), but on every reconnect after that, this
 * same GI snapshot instead diffs against the REAL, preserved last-known
 * values from before the disconnect - a genuine change made while
 * disconnected now correctly forwards with a real previousValue, and an
 * unchanged resend is correctly suppressed by the ordinary diff check, no
 * bootstrap logic involved. (This supersedes an earlier design where the
 * whole cache was wiped back to NULL on every reconnect via a since-deleted
 * `resetValueDiffCache` function - that made every reconnect look like a
 * fresh bootstrap, discarding the real last-known value and reporting
 * `previousValue: null` right when a real value existed a moment before.)
 * **A ReasonForInclusion real-change bit (DATA_CHANGE/QUALITY_CHANGE/
 * DATA_UPDATE) is deliberately NOT trusted as an unconditional bypass of this
 * check** - an earlier version of this code did trust it, but real-hardware
 * testing against a live IED proved that unsafe: the device repeatedly
 * tagged reports as DATA_CHANGE for a value that provably never changed
 * (confirmed via MmsReportEntry.previousValue itself showing
 * previousValue == value on hundreds of consecutive forwards), including on
 * GI-triggered snapshots (GI and DATA_CHANGE are independent, combinable
 * ReasonForInclusion bits, so a server can and did set both at once,
 * bypassing bootstrap-suppression the same way). `reason` is still carried
 * on MmsReportEntry as informational metadata, just never consulted for the
 * forward/drop decision.
 */
typedef struct {
    char* rcbReference;      /* owned copy, matches ReportControlBlockTarget.objectReference - lookup key */
    char** memberReferences; /* owned array of owned strings, index i matches MmsReportEntry[i] for this RCB */
    int memberCount;

    /* Gap 4: memberLeafReferences[i] is NULL if raw member i is already
     * leaf-level (its FCDA had a daName - nothing to decompose); otherwise an
     * owned array of memberLeafCounts[i] owned leaf-reference strings, from
     * IedModel_getDataSetMemberLeafReferences, in the order that must match
     * flattening member i's received MmsValue (see
     * MmsReportClientUtils_flattenStructure) - a mismatch between the two
     * counts at report time means don't decompose, fall back to the raw
     * single entry (see that function's own doc comment for why). */
    char*** memberLeafReferences;
    int* memberLeafCounts;

    /* Parallel to memberLeafReferences (same shape: memberLeafWireTypes[i] is
     * NULL unless raw member i decomposes, otherwise an owned array of
     * memberLeafCounts[i] DataAttributeTypes from
     * IedModel_getDataSetMemberLeafWireTypes) - each decomposed leaf's own
     * EXPECTED (SCL-declared) type. Passed into
     * reorderFlattenedToMatchReferences (mms_report_client_usecases.c) as a
     * disambiguation signal for the reorder - a leaf's expected type is only
     * ever used to resolve which remaining wire value belongs to it when
     * that type uniquely identifies exactly one remaining candidate (via
     * IedModel_dataAttributeTypeMatchesMmsType); it is NEVER used to reject
     * the decomposition outright (an earlier reject-on-mismatch gate here
     * was removed at explicit user request - see that function's own doc
     * comment). Confirmed against real production hardware that
     * memberLeafCounts matching alone is not sufficient - a real device's
     * actual wire order for a structured attribute (e.g. a DPC's "Pos") does
     * not have to match this daemon's locally-resolved SCL <DOType> order,
     * and a same-count-different-order mismatch silently mislabels every
     * leaf with no error (confirmed twice: q/stVal swapped on one device,
     * stVal/stSeld swapped on another). May be NULL (degrades to "no type
     * check, trust positional order" - the old behavior) if allocation
     * failed at build time, same OOM posture as every other field here. */
    DataAttributeType** memberLeafWireTypes;

    /* Value-diff cache (see this struct's own doc comment above - always
     * consulted, regardless of reason). One slot per *expanded leaf* position:
     * a non-decomposed member i occupies exactly 1 slot at
     * leafSlotOffsets[i]; a decomposed member i occupies memberLeafCounts[i]
     * consecutive slots starting at leafSlotOffsets[i]. totalLeafSlots is the
     * full flattened slot count (sum of every member's leaf count, or 1 for
     * non-decomposed members) - the allocated size of lastForwardedValues.
     * Each slot starts NULL (never forwarded yet); mutated in place by
     * MmsReportClientUseCases_buildReportRecord as reports are processed.
     * Once a slot receives its first real value, it is NEVER reset back to
     * NULL again for the rest of this client's lifetime (only
     * MmsReportClientUseCases_destroyMemberRefCacheEntry frees it, at
     * STOP_REPORTING/destroy time) - see this struct's own top comment. */
    int* leafSlotOffsets;
    int totalLeafSlots;
    MmsValue** lastForwardedValues;

    /* Parallel to lastForwardedValues (size totalLeafSlots) - slot i's
     * Dbpos-ness, resolved once at buildMemberRefCache time via
     * IedModel_getDataSetMemberSemantics/_getDataSetMemberLeafSemantics.
     * Unlike lastForwardedValues, never reset on reconnect (a DA's real SCL
     * type doesn't change across reconnects) - only
     * MmsReportClientUseCases_destroyMemberRefCacheEntry frees this. May be
     * NULL (degrades to IED_MODEL_DA_SEMANTIC_NONE everywhere) if allocation
     * failed at build time - same OOM posture as every other field here. */
    IedModelDaSemantic* leafSemantics;

    /* Set to true, once, at the end of the first report this RCB ever
     * processes (see buildEntries in mms_report_client_usecases.c) - purely
     * to gate the debug logging in shouldForwardAndUpdateCache: a NULL cache
     * slot found while this is still false is the expected, routine
     * first-ever-report case (nothing to log); a NULL slot found after this
     * is true should be structurally impossible under the current
     * never-reset design and is logged loudly on every occurrence as a bug
     * worth investigating. Explicitly initialized false in buildMemberRefCache
     * (mms_report_client_api.c) - this struct is malloc'd, not calloc'd, so
     * it is not zero-initialized for free. */
    bool everPopulated;

    /* Owned clone of the most recent ClientReport_getEntryId this RCB's
     * report adapter has seen (NULL until the first report carrying one
     * arrives - unbuffered RCBs never carry an EntryID at all, so this stays
     * NULL for their whole lifetime, same as any RCB whose reports simply
     * never set OptFlds.EntryId). Written by the report-adapter thread
     * (mms_report_client_report_adapter.c's onReport, unconditionally -
     * independent of whether that particular report survives the value-diff
     * filter, since EntryID tracks "durably received from the server," not
     * "passed our own forwarding decision") and read by the supervisor
     * thread (mms_report_client_connection.c's enableOneTarget, on every
     * (re)enable of a buffered RCB) - both sides MUST go through
     * handle->memberRefCacheLock, the same lock that already guards
     * lastForwardedValues. Passed to ClientReportControlBlock_setEntryId so
     * a reconnect resumes a buffered RCB's delivery from where this client
     * left off, instead of re-requesting the server's entire unacknowledged
     * backlog on every RptEna transition (confirmed against real hardware:
     * without this, a buffered RCB redelivers its full backlog on every
     * reconnect, and since the value-diff cache only remembers the single
     * last forwarded value, replaying the same multi-entry backlog again
     * defeats it - each redelivery's first entry looks "changed" relative to
     * wherever the cache landed after the previous redelivery's last entry).
     * Explicitly initialized NULL in buildMemberRefCache, freed by
     * MmsReportClientUseCases_destroyMemberRefCacheEntry. */
    MmsValue* lastEntryId;

    /* Owned copy of whichever dataset reference this entry's shape (every
     * field above) currently reflects, or NULL if not yet resolved from a
     * live connection. For a target with an SCL datasetReference
     * (ReportControlBlockTarget.datasetReference non-NULL), this is set once
     * in buildMemberRefCache from that same immutable value and never
     * rechecked - enableOneTarget never even reaches the tier-2/3 branch for
     * such a target, so this field is pure bookkeeping there, never compared
     * against anything.
     *
     * For a "Dyn" target (no SCL datasetReference), this is the single source
     * of truth for whether this entry's current shape is still valid on a
     * (re)connect: enableOneTarget (mms_report_client_connection.c) compares
     * the dataset name it actually ends up using this enable - whatever
     * mms_dataset_manager resolved for it this cycle (a pulled live/adopted
     * dataset, or its own deterministic self-created name) - against this field.
     * Equal means this entry's shape already matches (the common case on
     * every reconnect: the same live-assigned dataset stays put, or the same
     * deterministic self-created name is regenerated) - no rebuild needed.
     * Different means the previously-cached shape can no longer be trusted
     * for this RCB (a live-assigned dataset's identity genuinely changed
     * underneath the daemon, or a transition between tier 2 and tier 3) -
     * MmsReportClientConnection_refreshPulledMemberRefCache/
     * _ensureLnFallbackMemberRefCache (mms_report_client_connection.c) rebuild
     * every field above from scratch in that case, INCLUDING resetting
     * lastForwardedValues/leafSlotOffsets/everPopulated/lastEntryId back to a
     * fresh bootstrap state - a narrow, explicit exception to "the value-diff
     * cache is never reset" (see this struct's own top comment), justified
     * because a shape change invalidates slot INDICES themselves: slot 3
     * under the old shape and slot 3 under the new shape are not the same
     * wire position, so preserving old slot contents across a shape change
     * would silently misattribute a stale value to an unrelated new
     * position. Freed alongside rcbReference by
     * MmsReportClientUseCases_destroyMemberRefCacheEntry. */
    char* resolvedDatasetReference;
} MmsReportClientMemberRefCacheEntry;

/*
 * A single fully-decoded, fully-owned report. Delivered to the caller's
 * MmsReportClientCallback; the caller owns it after the callback returns and
 * must free it with MmsReportClient_destroyReportRecord.
 */
typedef struct {
    char* rcbReference;   /* owned copy, e.g. "Breaker1CB1/LLN0.BR.brcbMain" */
    bool buffered;
    char* rptId;           /* owned copy */

    bool hasEntryId;
    MmsValue* entryId;     /* owned deep copy (octet string), valid only if hasEntryId */

    bool hasTimestamp;
    uint64_t timestampMs;  /* ms since 1970-01-01 UTC, valid only if hasTimestamp */

    bool hasSeqNum;
    uint16_t seqNum;       /* valid only if hasSeqNum */

    MmsReportEntry* entries; /* owned array of entryCount elements */
    int entryCount;
} MmsReportRecord;

typedef void (*MmsReportClientCallback)(void* userParam, const MmsReportRecord* record);
typedef void (*MmsReportClientConnStateCallback)(void* userParam, MmsReportClientConnState state);
typedef void (*MmsReportClientRcbStatusCallback)(void* userParam, const char* rcbReference,
        bool enabled, IedClientError lastError);

typedef struct {
    uint32_t connectTimeoutMs;         /* 0 = library default */
    uint32_t requestTimeoutMs;         /* 0 = library default */
    uint32_t reconnectInitialDelayMs;  /* default 1000 */
    uint32_t reconnectMaxDelayMs;      /* default 30000 */
    const char* acseAuthPassword;      /* NULL = no ACSE authentication (default) - same
                                           borrowed-at-the-config-struct-level convention as
                                           SclBootstrapConfig's own field of the same name;
                                           MmsReportClient_create takes its own owned copy
                                           (handle->ownedAuthPassword) for the handle's whole
                                           lifetime, so the caller's buffer only needs to
                                           survive the _create() call itself. Applied
                                           unconditionally (no auth-then-retry dance - see
                                           mms_report_client_auth.h for why that's fine here). */
} MmsReportClientConfig;

/*
 * Internal representation. Defined here (rather than hidden behind an
 * additional internal-only header) because every file within this feature
 * needs field access, mirroring ied_model's struct sIedModelHandle convention:
 * opacity is enforced by which header is exposed (service/mms_report_client_api.h
 * is the only public one), not by hiding the struct.
 */
struct sMmsReportClientHandle {
    IedModelHandle iedModel; /* borrowed - caller retains ownership */
    char* host;              /* owned copy */
    int port;
    MmsReportClientConfig config;
    char* ownedAuthPassword; /* owned copy of config.acseAuthPassword, re-pointed onto config
                                 right after duplication - see MmsReportClientConfig's own
                                 field doc comment */

    IedConnection connection; /* owned */
    LinkedList targets;        /* owned: ReportControlBlockTarget* list, our own
                                  copy from IedModel_getReportSubscriptionTargets,
                                  cached for re-enable on reconnect */
    LinkedList memberRefCache; /* owned: MmsReportClientMemberRefCacheEntry* list,
                                   built once in MmsReportClient_start from ied_model's
                                   SCL dataset, one entry per target with a resolvable
                                   datasetReference - same lifetime as `targets`
                                   (survives reconnects, not rebuilt per-report). Each
                                   entry's own lastForwardedValues is likewise populated
                                   once (on that RCB's first-ever report) and PRESERVED
                                   for the client's whole lifetime - never reset on a
                                   (re-)enable - so a reconnect's GI/redelivered report
                                   diffs against the real, preserved last-known values
                                   from before the disconnect instead of a wiped-clean
                                   cache. See MmsReportClientMemberRefCacheEntry's own
                                   doc comment for the full design. */

    /* Owned. Everything about WHICH dataset each RCB should report on lives
     * behind this handle - discovering what already exists on the device
     * (SCL-static, live-assigned, foreign/leftover), planning whole-device
     * coverage, creating what's still missing, budgeting all of it against
     * SCL's declared capacity, and cleaning up its own leftovers. This feature
     * only consumes the answer: bind the resolved dataset to the RCB, enable
     * it, decode its reports.
     *
     * Created in MmsReportClient_start (once targets are known), destroyed in
     * MmsReportClient_destroy. It BORROWS this handle's iedModel, connection
     * and targets, so it must not outlive any of them. NULL if _start was
     * never called - every call site is NULL-safe, matching the pre-existing
     * "stop/destroy on a never-started client is a no-op" contract. */
    MmsDatasetManagerHandle datasetManager;

    MmsReportClientCallback reportCallback;
    void* reportCallbackParam;
    MmsReportClientConnStateCallback connStateCallback;
    void* connStateCallbackParam;
    MmsReportClientRcbStatusCallback rcbStatusCallback;
    void* rcbStatusCallbackParam;

    /* reconnect supervisor state */
    volatile bool stopRequested;
    volatile bool connectionLostSignal;
    volatile bool supervisorExited;
    Semaphore wakeSignal;
    Thread supervisorThread;
    uint32_t currentBackoffMs;

    /* Edge-detection flag for MMS_REPORT_CLIENT_CONNECTION_REJECTED: true once
     * connStateCallback has been fired with that state since the last successful
     * IedConnection_connect() - reset back to false the moment a connect attempt
     * next succeeds. Without this, a device stuck rejecting every attempt would
     * push an identical CONNECTION_STATUS notification every single backoff cycle
     * forever (as often as ~1s at the initial tier) - see supervisorLoop. */
    bool connectionRejectedSignaled;

    /* Binary mutex (Semaphore_create(1)) guarding every memberRefCache
     * entry's lastForwardedValues slots. Historically also guarded the
     * reconnect-supervisor thread's own cache reset (enableOneTarget used to
     * call a since-deleted MmsReportClientUseCases_resetValueDiffCache on
     * every (re-)enable) racing the report-adapter thread's concurrent
     * updates - confirmed as a real root cause (combined with a since-fixed
     * reconnect "storm" in supervisorLoop) of a real-hardware flood of
     * forwarded MMS_REPORT messages with previousValue == value after a
     * device reconnect. That reset no longer exists at all (the cache is
     * populated once and preserved forever - see
     * MmsReportClientMemberRefCacheEntry's own doc comment), so the
     * report-adapter thread (onReport -> MmsReportClientUseCases_buildReportRecord)
     * is currently this cache's only writer - the lock is kept anyway as
     * cheap, uncontended insurance against any future writer reappearing on
     * another thread. Created/destroyed alongside wakeSignal. */
    Semaphore memberRefCacheLock;
};

typedef struct sMmsReportClientHandle* MmsReportClientHandle;

#endif /* MMS_REPORT_CLIENT_TYPES_H_ */
