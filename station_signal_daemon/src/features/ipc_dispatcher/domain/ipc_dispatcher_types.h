#ifndef IPC_DISPATCHER_TYPES_H_
#define IPC_DISPATCHER_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Domain vocabulary for this feature is entirely its own (IpcScalarValue/
 * IpcQuality/IpcMessage - the JSON contract itself), not borrowed from one
 * vendor lib - unlike mms_report_client/goose_subscriber, ipc_dispatcher has
 * two unrelated third-party inputs (MmsValue) and two unrelated third-party
 * outputs (cJSON, libwebsockets), none of which is "the" domain. Zero
 * third-party includes here by design - see utils/ipc_dispatcher_value_codec.h
 * for the one place MmsValue/Quality are touched, and data/ for cJSON/
 * libwebsockets.
 */

typedef enum {
    IPC_DISPATCHER_OK = 0,
    IPC_DISPATCHER_ERR_INVALID_ARGUMENT,
    IPC_DISPATCHER_ERR_OUT_OF_MEMORY,
    IPC_DISPATCHER_ERR_THREAD_CREATE_FAILED,
    IPC_DISPATCHER_ERR_SOCKET_BIND_FAILED,
    IPC_DISPATCHER_ERR_ALREADY_RUNNING
} IpcDispatcherError;

/* Mirrors iec61850_common.h's QUALITY_VALIDITY_* 2-bit field, named -
 * see utils/ipc_dispatcher_value_codec.h for the decode path. */
typedef enum {
    IPC_QUALITY_GOOD = 0,
    IPC_QUALITY_RESERVED,
    IPC_QUALITY_INVALID,
    IPC_QUALITY_QUESTIONABLE
} IpcQualityValidity;

/*
 * detailFlags is the raw Quality bitset (iec61850_common.h's
 * QUALITY_DETAIL_... / QUALITY_TEST / QUALITY_OPERATOR_BLOCKED /
 * QUALITY_SOURCE_SUBSTITUTED / QUALITY_DERIVED bits, plus the validity bits
 * already named above) copied verbatim rather than exposed as individually
 * named booleans in v1 - additive later (a frontend-visible detail flag) is
 * not a breaking JSON-contract change, but un-inventing one would be.
 */
typedef struct {
    IpcQualityValidity validity;
    uint16_t detailFlags;
} IpcQuality;

typedef enum {
    IPC_SCALAR_BOOL = 0,
    IPC_SCALAR_INT64,
    IPC_SCALAR_UINT64,
    IPC_SCALAR_DOUBLE,
    IPC_SCALAR_STRING,
    IPC_SCALAR_RAW /* fallback for a type not decoded in v1, or a NULL MmsValue*
                       - value.str is an owned, human-readable placeholder,
                       e.g. "<unsupported:MMS_OCTET_STRING>" / "<null>" -
                       never silently dropped, see utils/ipc_dispatcher_value_codec.h */
} IpcScalarType;

typedef struct {
    IpcScalarType type;
    union {
        bool b;
        int64_t i64;
        uint64_t u64;
        double d;
        char* str; /* owned; used by IPC_SCALAR_STRING and IPC_SCALAR_RAW only */
    } value;
} IpcScalarValue;

typedef enum {
    IPC_SOURCE_MMS_REPORT = 0,
    IPC_SOURCE_GOOSE
} IpcSourceType;

/* One value entry, with its "q" sibling (if any) already paired in - see
 * domain/ipc_dispatcher_usecases.h's pairing algorithm. */
typedef struct {
    char* reference; /* owned copy - the VALUE entry's own reference, never the q sibling's */
    IpcScalarValue value;
    bool hasQuality;
    IpcQuality quality; /* valid only if hasQuality */

    /* Previous value/quality for this same reference - lets the frontend
     * show what changed FROM, not just the new value. Sourced from
     * MmsReportEntry.previousValue/GooseSubscriberEntry.previousValue (the
     * value-diff cache's pre-update snapshot) via the same value/quality
     * codec used for the current value - see the mms/goose adapters. Absent
     * only in the narrow, pre-existing structural case where the source
     * entry had no cache slot to diff against at all (see
     * MmsReportEntry.previousValue's own doc comment) - NOT a routine
     * outcome, since GI/bootstrap seeding means a previous value is present
     * in essentially every real case once a device has been reporting for
     * any length of time. */
    bool hasPreviousValue;
    IpcScalarValue previousValue; /* valid only if hasPreviousValue */
    bool hasPreviousQuality;
    IpcQuality previousQuality; /* valid only if hasPreviousQuality */

    /* Descriptive label for a genuine Dbpos-typed value (IEC 61850-7-3:
     * "intermediate-state"/"off"/"on"/"bad-state"), additive alongside the
     * existing raw numeric `value` - never replaces it. Present only when
     * ied_model's SCL-derived semantics table confirmed the source DA's real
     * bType was "Dbpos" (see IedModelDaSemantic) - never guessed from the
     * wire bitstring alone. previousLabel mirrors this for previousValue,
     * when both a previous value and the Dbpos semantic are present. Both
     * are non-owned pointers into static string-literal storage - never
     * freed. */
    bool hasLabel;
    const char* label; /* valid only if hasLabel - NOT owned, never freed */
    bool hasPreviousLabel;
    const char* previousLabel; /* valid only if hasPreviousLabel - NOT owned, never freed */
} IpcDataPoint;

/*
 * Optional, bundled "extra" per-data-point arrays for
 * IpcDispatcherUseCases_assembleMessage - kept as a separate struct rather
 * than doubling that function's own already-long parameter list further.
 * Every array (if the struct itself is non-NULL) must have room for
 * `pointCount` elements, index-aligned with pointReferences/pointValues/
 * pointHasQuality/pointQuality. Passing NULL for the whole struct (or for
 * any one array within it) means "no data for this field on any point" -
 * every hasPreviousValue/hasPreviousQuality/hasLabel/hasPreviousLabel in the
 * resulting IpcMessage stays false.
 */
typedef struct {
    const bool* pointHasPreviousValue;
    const IpcScalarValue* pointPreviousValue;
    const bool* pointHasPreviousQuality;
    const IpcQuality* pointPreviousQuality;
    const bool* pointHasLabel;
    const char* const* pointLabel;
    const bool* pointHasPreviousLabel;
    const char* const* pointPreviousLabel;
} IpcDataPointExtras;

typedef struct {
    IpcSourceType sourceType;
    char* sourceReference; /* owned copy: rcbReference (MMS) or goCbRef (GOOSE) - may be NULL */
    bool hasBuffered;       /* true only for IPC_SOURCE_MMS_REPORT */
    bool buffered;           /* valid only if hasBuffered */
    bool hasTimestamp;       /* mirrors MmsReportRecord.hasTimestamp; GOOSE always sets this true */
    uint64_t timestampMs;    /* valid only if hasTimestamp */
    IpcDataPoint* dataPoints; /* owned array */
    int dataPointCount;
} IpcMessage;

typedef struct {
    uint16_t port;           /* default 8765, loopback-only bind - see service/ipc_dispatcher_api.h */
    int ringBufferCapacity;  /* default 256 */
    int maxConnections;      /* default 16 */
} IpcDispatcherConfig;

/*
 * One recently-forwarded data point's content plus which source produced it -
 * sourceId is the MMS rcbReference or GOOSE goCbRef that forwarded it. Used
 * by IpcDispatcherUseCases_shouldForwardWithinProtocol (domain/ipc_dispatcher_usecases.h)
 * to catch the SAME real change forwarded redundantly by two DIFFERENT
 * sources of the SAME protocol (e.g. three MMS RCBs all covering the same
 * underlying reference) - see that function's own doc comment. Two separate
 * instances exist (data/ipc_dispatcher_dedup_cache.c wraps one each for MMS
 * and GOOSE, on sIpcDispatcherHandle below) - MMS and GOOSE are never
 * cross-checked against each other, since a real SCL can legitimately wire
 * both protocols to the same point on purpose (two independent wire paths),
 * unlike two RCBs/GoCBs of the SAME protocol landing on the same content,
 * which is the daemon's own dataset-assignment redundancy this exists to
 * catch. A bounded ring, not a single slot - a single "last forwarded" slot
 * (this codebase's own earlier per-feature designs, both since superseded by
 * this shared cache) is too easily defeated by interleaved traffic from
 * unrelated sources: an unrelated report/frame landing between two
 * duplicates clobbers the one slot before the real duplicate arrives.
 */
typedef struct {
    char* sourceId;   /* owned; NULL means an empty slot */
    char* reference;  /* owned */
    IpcScalarValue value; /* owned deep copy - see cloneScalarValue in ipc_dispatcher_usecases.c */
    bool hasQuality;
    IpcQuality quality;
} IpcDispatcherDedupEntry;

/* Ring capacity - mirrors GOOSE_SUBSCRIBER_RECENT_FORWARD_CAPACITY's own
 * proven-sufficient sizing (goose_subscriber_types.h), generous enough that
 * an interleaved burst of unrelated traffic from other sources doesn't evict
 * the entry a later duplicate needs to match against. */
#define IPC_DISPATCHER_DEDUP_CAPACITY 128

typedef struct {
    IpcDispatcherDedupEntry history[IPC_DISPATCHER_DEDUP_CAPACITY];
    int count;    /* valid slots filled so far, caps at capacity */
    int nextSlot; /* ring write cursor - wraps and overwrites the oldest slot once full */
} IpcDispatcherDedupCache;

/*
 * Opaque forward declarations only - full struct defs live entirely inside
 * data/ipc_dispatcher_ring_buffer.c / data/ipc_dispatcher_ws_server.c (unlike
 * every sibling feature's struct s*Handle, nothing outside those two .c files
 * needs field access, so there's no reason to expose the layout here). Kept
 * as bare tags (not #include'd) so this header never pulls in hal_thread.h/
 * libwebsockets.h - dependency direction stays data/utils -> domain, never
 * the reverse.
 */
struct sIpcDispatcherRingBuffer;
struct sIpcDispatcherWsServer;
struct sIpcDispatcherDedupCache;

/*
 * Internal representation. Defined here (rather than hidden behind an
 * additional internal-only header) because every file within this feature
 * needs field access, mirroring every sibling feature's struct s*Handle
 * convention: opacity is enforced by which header is exposed
 * (service/ipc_dispatcher_api.h is the only public one), not by hiding the
 * struct.
 */
struct sIpcDispatcherHandle {
    IpcDispatcherConfig config;
    struct sIpcDispatcherRingBuffer* ringBuffer; /* owned, created in IpcDispatcher_create */
    struct sIpcDispatcherWsServer* wsServer;     /* owned, created/destroyed in _start/_stop - NULL when not running */
    volatile bool running;

    /* One dedup cache per protocol - see IpcDispatcherDedupCache's own doc
     * comment for why MMS and GOOSE are never cross-checked against each
     * other. Owned, created in IpcDispatcher_create, destroyed in _destroy. */
    struct sIpcDispatcherDedupCache* mmsDedupCache;
    struct sIpcDispatcherDedupCache* gooseDedupCache;
};

typedef struct sIpcDispatcherHandle* IpcDispatcherHandle;

#endif /* IPC_DISPATCHER_TYPES_H_ */
