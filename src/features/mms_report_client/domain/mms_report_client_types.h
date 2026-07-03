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
 */
typedef struct {
    char* rcbReference;      /* owned copy, matches ReportControlBlockTarget.objectReference - lookup key */
    char** memberReferences; /* owned array of owned strings, index i matches MmsReportEntry[i] for this RCB */
    int memberCount;
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
                                   (survives reconnects, not rebuilt per-report) */

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
