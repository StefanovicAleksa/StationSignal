#ifndef MMS_REPORT_CLIENT_TYPES_H_
#define MMS_REPORT_CLIENT_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "linked_list.h"
#include "iec61850_client.h"
#include "mms_value.h"
#include "hal_thread.h"
#include "features/ied_model/service/ied_model_api.h"

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
    MMS_REPORT_CLIENT_ERR_THREAD_CREATE_FAILED
} MmsReportClientError;

typedef enum {
    MMS_REPORT_CLIENT_DISCONNECTED = 0,
    MMS_REPORT_CLIENT_CONNECTING,
    MMS_REPORT_CLIENT_CONNECTED,
    MMS_REPORT_CLIENT_RECONNECT_BACKOFF
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
 * Event filtering is a hybrid: an entry whose ReasonForInclusion carries a
 * real-change bit (DATA_CHANGE/QUALITY_CHANGE/DATA_UPDATE) is always
 * forwarded, trusting the server's own word. An entry with no such bit
 * (INTEGRITY/GI-only, or UNKNOWN/NOT_INCLUDED on servers that never populate
 * reason-for-inclusion at all) is instead gated by the per-position
 * lastForwardedValues cache below - forwarded only if its value differs from
 * the last one actually sent for that exact wire position, or if nothing has
 * ever been forwarded for it yet (a NULL slot always survives). That NULL-
 * survives rule is what lets the one-time startup GI snapshot deliver each
 * position's initial value to the caller, while still catching a real
 * device's periodic/unflagged re-sends of an unchanged value.
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

    /* Value-diff cache (the hybrid gate's non-real-change branch - see this
     * struct's own doc comment above). One slot per *expanded leaf* position:
     * a non-decomposed member i occupies exactly 1 slot at
     * leafSlotOffsets[i]; a decomposed member i occupies memberLeafCounts[i]
     * consecutive slots starting at leafSlotOffsets[i]. totalLeafSlots is the
     * full flattened slot count (sum of every member's leaf count, or 1 for
     * non-decomposed members) - the allocated size of lastForwardedValues.
     * Each slot starts NULL (never forwarded yet); mutated in place by
     * MmsReportClientUseCases_buildReportRecord as reports are processed. */
    int* leafSlotOffsets;
    int totalLeafSlots;
    MmsValue** lastForwardedValues;
} MmsReportClientMemberRefCacheEntry;

/*
 * One (reference, value) pair kept by MmsReportClientCrossRcbDedupCache -
 * deliberately not MmsReportEntry itself (which also carries a `reason` this
 * comparison doesn't care about). reference/value are both owned.
 */
typedef struct {
    char* reference;
    MmsValue* value;
} MmsReportClientDedupEntry;

/*
 * Cross-RCB duplicate-content suppression: a deep copy of the last record
 * this client actually forwarded to reportCallback, from ANY RCB - not to be
 * confused with MmsReportClientMemberRefCacheEntry's per-RCB value-diff
 * cache, which only ever compares a report against that SAME RCB's own
 * history. Some real devices configure multiple reserved/redundant RCB
 * instances on the same LN/dataset for multi-client redundancy (e.g.
 * "urcbA01"/"urcbB01") - both instances report the exact same underlying
 * event at nearly the same moment, and since each RCB's own value-diff cache
 * starts independent, both reports survive their own per-RCB hybrid filter
 * and would otherwise both reach the websocket as apparent duplicates.
 * MmsReportClientUseCases_shouldForwardAcrossRcb is the single decision
 * point: a record whose (reference, value) content exactly matches this
 * cache AND whose rcbReference differs from the one that produced it is
 * suppressed; anything else (first-ever content, a genuine change, or a
 * repeat from the SAME rcbReference - already the per-RCB filter's own
 * concern) updates this cache and is forwarded. Persists across reconnects,
 * same as the per-RCB caches - deliberately not reset in enableOneTarget
 * (unlike MmsReportClientMemberRefCacheEntry.lastForwardedValues), since the
 * duplicate-suppression concern this cache addresses is orthogonal to "did
 * this RCB's own state resync on reconnect".
 */
typedef struct {
    char* rcbReference;                  /* owned; NULL means nothing forwarded yet */
    MmsReportClientDedupEntry* entries;   /* owned array of entryCount owned entries */
    int entryCount;
} MmsReportClientCrossRcbDedupCache;

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
    bool generalInterrogationOnEnable; /* default true - see MmsReportClientConfig_defaults() */
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
                                   entry's own lastForwardedValues IS explicitly reset
                                   on every successful (re-)enable though - see
                                   MmsReportClientUseCases_resetValueDiffCache, called
                                   from enableOneTarget - so a reconnect's GI snapshot
                                   is never diffed against stale pre-disconnect values. */
    MmsReportClientCrossRcbDedupCache crossRcbDedupCache; /* zero-initialized by calloc in
                                   MmsReportClient_create (NULL rcbReference means "nothing
                                   forwarded yet") - see its own doc comment above */

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
};

typedef struct sMmsReportClientHandle* MmsReportClientHandle;

#endif /* MMS_REPORT_CLIENT_TYPES_H_ */
