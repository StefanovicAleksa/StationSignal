#ifndef IPC_DISPATCHER_VALUE_CODEC_H_
#define IPC_DISPATCHER_VALUE_CODEC_H_

#include "mms_value.h"
#include "iec61850_common.h"
#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"
#include "features/ied_model/service/ied_model_api.h"

/*
 * MmsValue -> IpcScalarValue / IpcQuality decode. Lives in utils/ (not
 * domain/) precisely because it touches libiec61850's MmsValue/Quality
 * directly - the one shared third-party-touching helper used by both the
 * mms_report_client and goose_subscriber adapters (data/).
 */

/*
 * Converts one scalar MmsValue by MmsValue_getType():
 *   MMS_BOOLEAN                     -> IPC_SCALAR_BOOL   (MmsValue_getBoolean)
 *   MMS_INTEGER                     -> IPC_SCALAR_INT64  (MmsValue_toInt64)
 *   MMS_UNSIGNED                    -> IPC_SCALAR_UINT64 (MmsValue_toUint32,
 *                                       widened - no MmsValue_toUint64 exists
 *                                       in the vendored header; a 32-bit
 *                                       ceiling on unsigned values is a known
 *                                       v1 limitation)
 *   MMS_FLOAT                       -> IPC_SCALAR_DOUBLE (MmsValue_toDouble)
 *   MMS_VISIBLE_STRING / MMS_STRING -> IPC_SCALAR_STRING (MmsValue_toString, strdup'd)
 *   MMS_UTC_TIME                    -> IPC_SCALAR_UINT64 (MmsValue_getUtcTimeInMs) -
 *                                       unexpected as a plain "value" entry
 *                                       (record-level timestamp is separate)
 *                                       but handled defensively, not asserted against
 *   MMS_BIT_STRING                  -> IPC_SCALAR_UINT64 (MmsValue_getBitStringAsInteger,
 *                                       little-endian bit order per that
 *                                       function's own doc comment) - covers
 *                                       CODEDENUM-typed value DAs (e.g.
 *                                       Dbpos/Tcmd, see IedModelUtils_mapBType)
 *                                       that wire-encode as a bitstring, not
 *                                       just quality's own bitstring (which
 *                                       never reaches this function at all -
 *                                       pairQuality excludes every "q"-named
 *                                       entry from ever being treated as a
 *                                       value, routing it to _decodeQuality
 *                                       below instead). Deliberately a raw
 *                                       integer, not a named enum string,
 *                                       for every CODEDENUM subtype uniformly
 *                                       (Dbpos, Tcmd, or otherwise) - this
 *                                       function has no way to know which
 *                                       specific CODEDENUM a given bitstring
 *                                       represents, so guessing a decoded
 *                                       label without per-type verification
 *                                       would violate this repo's own "don't
 *                                       guess IEC 61850 semantics" rule - the
 *                                       raw bit pattern is always correct
 *                                       regardless of which CODEDENUM it is.
 *                                       KNOWN CAVEAT for genuine Dbpos values
 *                                       specifically: MmsValue_getBitStringAsInteger's
 *                                       bit order does NOT match
 *                                       Dbpos_fromMmsValue's own decode
 *                                       (confirmed empirically - ordinals
 *                                       DBPOS_OFF/DBPOS_ON come out swapped
 *                                       between the two). Callers that also
 *                                       call _decodeDbposLabel below and get
 *                                       true back MUST override this raw
 *                                       value with (uint64_t)
 *                                       Dbpos_fromMmsValue(value) instead, so
 *                                       the numeric value and the label never
 *                                       contradict each other - see the mms/
 *                                       goose adapters for the exact pattern.
 *   everything else (MMS_STRUCTURE/MMS_ARRAY/MMS_OCTET_STRING/
 *   MMS_GENERALIZED_TIME/MMS_BINARY_TIME/MMS_BCD/MMS_OBJ_ID/
 *   MMS_DATA_ACCESS_ERROR)          -> IPC_SCALAR_RAW, value.str = owned
 *                                       "<unsupported:...>" placeholder -
 *                                       structure/array nesting is out of
 *                                       scope since dataset FCDA entries are
 *                                       individual DA leaves, not whole DOs.
 * value == NULL (GooseSubscriberEntry.value can legitimately be NULL) ->
 *   IPC_SCALAR_RAW, value.str = "<null>".
 * STRING/RAW variants are always strdup'd - never a dangling/unowned pointer
 * into the source MmsValue.
 */
IpcScalarValue
IpcDispatcherValueCodec_convert(const MmsValue* value);

/* Frees a scalar produced by _convert (only STRING/RAW own anything). Safe
 * to call on any value this module produced, including non-owning variants
 * (no-op then). */
void
IpcDispatcherValueCodec_freeScalar(IpcScalarValue* scalar);

/*
 * Decodes a "q" entry's wire-encoded MMS_BIT_STRING MmsValue via
 * Quality_fromMmsValue(value) (iec61850_common.h - decodes the bitstring
 * directly), maps Quality_getValidity's Validity to IpcQualityValidity, and
 * copies the full raw Quality bitset (including the validity bits already
 * named above) verbatim into outQuality->detailFlags - see
 * ipc_dispatcher_types.h's IpcQuality doc comment for why the detail/test/
 * substituted/derived bits aren't individually named in v1. Returns false
 * (outQuality left zeroed) if value is NULL or MmsValue_getType(value) !=
 * MMS_BIT_STRING - caller must check before treating the entry as quality.
 */
bool
IpcDispatcherValueCodec_decodeQuality(const MmsValue* value, IpcQuality* outQuality);

/*
 * Decodes a genuine Dbpos-typed value into its IEC 61850-7-3 label
 * (0=intermediate-state, 1=off, 2=on, 3=bad-state) via libiec61850's
 * Dbpos_fromMmsValue (iec61850_common.h). *outLabel is set to a pointer into
 * static string-literal storage (never owned, never freed by the caller).
 *
 * Returns false (outLabel untouched) unless semantic ==
 * IED_MODEL_DA_SEMANTIC_DBPOS AND value is non-NULL AND
 * MmsValue_getType(value) == MMS_BIT_STRING - this is the only place in this
 * codebase that decodes a CODEDENUM bitstring into a named label, and only
 * because the caller has already verified (via ied_model's SCL-derived
 * semantic hint, threaded through MmsReportEntry.semantic/
 * GooseSubscriberEntry.semantic) that this specific bitstring genuinely IS a
 * Dbpos, not a guess from the wire type alone (Tcmd shares the same wire
 * representation but a different meaning - see IedModelUtils_mapBType's own
 * doc comment). Does NOT replace the existing raw IPC_SCALAR_UINT64 value
 * produced by _convert - both are always emitted side by side.
 */
bool
IpcDispatcherValueCodec_decodeDbposLabel(IedModelDaSemantic semantic, const MmsValue* value, const char** outLabel);

#endif /* IPC_DISPATCHER_VALUE_CODEC_H_ */
