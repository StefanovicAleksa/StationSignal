#ifndef IPC_DISPATCHER_JSON_WRITER_H_
#define IPC_DISPATCHER_JSON_WRITER_H_

#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"
#include "features/mms_report_client/service/mms_report_client_api.h"

/*
 * IpcMessage -> JSON text via cJSON. The only file in this feature that
 * touches cJSON.h. No parse counterpart exists anywhere (push-only).
 *
 * Envelope shape (stable contract per CLAUDE.md's "treat message shape as a
 * stable contract" rule):
 *
 *   {
 *     "schemaVersion": 1,
 *     "type": "MMS_REPORT" | "GOOSE",
 *     "source": { "rcbReference": "...", "buffered": true }   (MMS_REPORT)
 *             | { "goCbRef": "..." }                           (GOOSE)
 *     "hasTimestamp": true,
 *     "timestampMs": 1751520000123,   -- present only if hasTimestamp is true
 *     "dataPoints": [
 *       {
 *         "reference": "...",
 *         "value": <bool | number | string>,
 *         "quality": { "validity": "GOOD", "detailFlags": 0 } | null
 *       }
 *     ]
 *   }
 *
 * Known caveat: cJSON numbers are double-backed, so an IPC_SCALAR_INT64/
 * UINT64 value beyond 2^53 loses precision. Acceptable for GGIO indications/
 * measurements (booleans/small integers/floats) reachable today.
 */

/* Returns a heap-allocated, NUL-terminated JSON string (cJSON_PrintUnformatted -
 * no pretty-printing, this is a wire payload) - caller owns it (free()).
 * Returns NULL if message is NULL or on allocation failure. */
char*
IpcDispatcherJsonWriter_write(const IpcMessage* message);

/*
 * Additive envelope, same "type" discriminator convention as above:
 *
 *   { "schemaVersion": 1, "type": "CONNECTION_STATUS", "status": "CONNECTION_REJECTED" }
 *
 * Only ever emitted for MMS_REPORT_CLIENT_CONNECTION_REJECTED - every other
 * MmsReportClientConnState value returns NULL (no-op): this stream deliberately
 * doesn't surface routine CONNECTING/CONNECTED/backoff churn, only the one
 * diagnostic state a caller can act on. See MmsReportClientConnState's own doc
 * comment (mms_report_client_types.h) for exactly what CONNECTION_REJECTED does
 * and does not mean. Caller owns the returned string (free()).
 */
char*
IpcDispatcherJsonWriter_writeConnectionStatus(MmsReportClientConnState state);

#endif /* IPC_DISPATCHER_JSON_WRITER_H_ */
