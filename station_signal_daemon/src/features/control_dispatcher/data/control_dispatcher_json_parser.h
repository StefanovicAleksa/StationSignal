#ifndef CONTROL_DISPATCHER_JSON_PARSER_H_
#define CONTROL_DISPATCHER_JSON_PARSER_H_

#include <stddef.h>
#include "features/control_dispatcher/domain/control_dispatcher_types.h"

typedef enum {
    CONTROL_PARSE_OK = 0,
    CONTROL_PARSE_ERR_MALFORMED_JSON,      /* cJSON_Parse itself returned NULL, or the root
                                               wasn't a JSON object */
    CONTROL_PARSE_ERR_MISSING_REQUEST_ID,  /* "requestId" missing or not a string */
    CONTROL_PARSE_ERR_UNKNOWN_ACTION,      /* "action" missing, not a string, or not one of the
                                               known action names */
    CONTROL_PARSE_ERR_INVALID_PARAMS       /* action recognized, but "params" is missing/not an
                                               object, or a required field within it is
                                               missing/wrong-typed/empty */
} ControlParseError;

/*
 * Parses one raw inbound websocket text frame into a fully owned
 * ControlRequest (see domain/control_dispatcher_types.h). This is the FIRST
 * cJSON_Parse call in this codebase's production code (see CLAUDE.md's cJSON
 * bullet, which currently says push-only/serialize-only - now inaccurate) -
 * every field is defensively type/NULL-checked before use, since this is
 * untrusted network input, unlike every existing cJSON use elsewhere in this
 * repo (which only ever serializes trusted, internally-generated data).
 *
 * Always fills outRecoveredRequestId/outRecoveredAction with best-effort
 * owned copies (possibly NULL) for error-response echoing, even on failure -
 * see this feature's own Architecture bullet for the envelope's error-echo
 * rules; caller (control_dispatcher_ws_server.c) frees them once the error
 * response has been built. On CONTROL_PARSE_OK, *outRequest is a fully
 * owned, heap-allocated ControlRequest ready for the request queue (caller
 * eventually calls ControlDispatcherRequest_destroy on it) and
 * outRecoveredRequestId/outRecoveredAction are both set to NULL (the
 * request itself already carries requestId; action is implied by
 * request->type).
 */
ControlParseError
ControlDispatcherJsonParser_parse(const char* rawJson, size_t len, char** outRecoveredRequestId,
        char** outRecoveredAction, ControlRequest** outRequest);

#endif /* CONTROL_DISPATCHER_JSON_PARSER_H_ */
