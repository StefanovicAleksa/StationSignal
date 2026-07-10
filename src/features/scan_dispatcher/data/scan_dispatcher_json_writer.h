#ifndef SCAN_DISPATCHER_JSON_WRITER_H_
#define SCAN_DISPATCHER_JSON_WRITER_H_

#include "features/scan_dispatcher/domain/scan_dispatcher_types.h"

/*
 * ScanDeviceFoundEvent -> JSON text via cJSON. The only file in this feature
 * that touches cJSON.h (mirrors ipc_dispatcher_json_writer.h's own split).
 * No parse counterpart exists anywhere (push-only).
 *
 * Envelope shape (stable contract per CLAUDE.md's "treat message shape as a
 * stable contract" rule):
 *
 *   {
 *     "schemaVersion": 1,
 *     "type": "SCAN_RESULT",
 *     "scanId": 1,
 *     "host": "192.168.1.50",
 *     "mmsPort": 102,
 *     "discoveredAtMs": 1751520000123
 *   }
 *
 * Known caveat: cJSON numbers are double-backed, so scanId/discoveredAtMs
 * beyond 2^53 lose precision - harmless at these magnitudes (a monotonic
 * per-process scanId counter, a wall-clock ms timestamp), same accepted
 * caveat ipc_dispatcher_json_writer.h already documents for its own int64
 * fields.
 */

/* Returns a heap-allocated, NUL-terminated JSON string (cJSON_PrintUnformatted -
 * no pretty-printing, this is a wire payload) - caller owns it (free()).
 * Returns NULL if event is NULL or on allocation failure. */
char*
ScanDispatcherJsonWriter_write(const ScanDeviceFoundEvent* event);

#endif /* SCAN_DISPATCHER_JSON_WRITER_H_ */
