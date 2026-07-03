#ifndef IPC_DISPATCHER_VALUE_CODEC_H_
#define IPC_DISPATCHER_VALUE_CODEC_H_

#include "mms_value.h"
#include "iec61850_common.h"
#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"

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
 *   everything else (MMS_STRUCTURE/MMS_ARRAY/MMS_BIT_STRING/MMS_OCTET_STRING/
 *   MMS_GENERALIZED_TIME/MMS_BINARY_TIME/MMS_BCD/MMS_OBJ_ID/
 *   MMS_DATA_ACCESS_ERROR)          -> IPC_SCALAR_RAW, value.str = owned
 *                                       "<unsupported:...>" placeholder -
 *                                       today's reachable dataset entries
 *                                       (ied_simulator's sim_types.h) are
 *                                       MMS_BOOLEAN (SPS stVal) +
 *                                       MMS_BIT_STRING (q, handled via
 *                                       _decodeQuality, not this path);
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

#endif /* IPC_DISPATCHER_VALUE_CODEC_H_ */
