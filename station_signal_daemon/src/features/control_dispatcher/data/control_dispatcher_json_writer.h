#ifndef CONTROL_DISPATCHER_JSON_WRITER_H_
#define CONTROL_DISPATCHER_JSON_WRITER_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Builds this feature's outbound JSON response envelope (stable contract -
 * see CLAUDE.md's control_dispatcher Architecture bullet for the full
 * example):
 *   { schemaVersion, requestId, action, success, result, error }
 * requestId/action may be NULL (written out as JSON null) when the inbound
 * message couldn't be parsed far enough to recover them. Every function
 * returns an owned, heap-allocated JSON string (caller frees), or NULL on
 * cJSON allocation failure only (extremely unlikely).
 */

/*
 * mmsAvailable/gooseAvailable report which of MMS reporting / GOOSE
 * subscription this device's SCL actually declared targets for - a device
 * with only one of the two now still succeeds START_REPORTING (see
 * orchestration's own ORCHESTRATION_ERR_NO_CAPABILITIES doc comment), so the
 * caller (frontend, via the API) needs this to know which stream(s) to
 * expect data on.
 */
char*
ControlDispatcherJsonWriter_writeStartSuccess(const char* requestId, uint64_t deviceId, uint16_t wsPort,
        bool mmsAvailable, bool gooseAvailable);

char*
ControlDispatcherJsonWriter_writeStopSuccess(const char* requestId, uint64_t deviceId);

char*
ControlDispatcherJsonWriter_writeScanStartSuccess(const char* requestId, uint64_t scanId);

char*
ControlDispatcherJsonWriter_writeScanStopSuccess(const char* requestId, uint64_t scanId);

/*
 * errorCode is a stable string (e.g. "INVALID_ARGUMENT", "HOST_ALREADY_RUNNING",
 * "ORCHESTRATION_FAILED", "DEVICE_NOT_FOUND", "START_IN_PROGRESS",
 * "PORT_EXHAUSTED", "MALFORMED_REQUEST", "UNKNOWN_ACTION", "SERVER_BUSY").
 * action may be NULL (unknown/unrecoverable). stage/detail are optional
 * (NULL omits that key from the JSON "error" object entirely) - populated
 * only for ORCHESTRATION_FAILED, per this feature's own error-mapping logic
 * in domain/control_dispatcher_usecases.c.
 */
char*
ControlDispatcherJsonWriter_writeError(const char* requestId, const char* action, const char* errorCode,
        const char* message, const char* stage, const char* detail);

#endif /* CONTROL_DISPATCHER_JSON_WRITER_H_ */
