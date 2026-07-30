#include "cJSON.h"
#include "stdbool_compat.h"
#include "features/control_dispatcher/data/control_dispatcher_json_writer.h"

static cJSON*
newEnvelope(const char* requestId, const char* action, bool success) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "schemaVersion", 1);
    if (requestId) cJSON_AddStringToObject(root, "requestId", requestId);
    else cJSON_AddNullToObject(root, "requestId");
    if (action) cJSON_AddStringToObject(root, "action", action);
    else cJSON_AddNullToObject(root, "action");
    cJSON_AddBoolToObject(root, "success", success);

    return root;
}

static char*
finish(cJSON* root) {
    if (!root) return NULL;

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char*
ControlDispatcherJsonWriter_writeStartSuccess(const char* requestId, uint64_t deviceId, uint16_t wsPort,
        bool mmsAvailable, bool gooseAvailable) {
    cJSON* root = newEnvelope(requestId, "START_REPORTING", true);
    if (!root) return NULL;

    cJSON* result = cJSON_AddObjectToObject(root, "result");
    if (result) {
        cJSON_AddNumberToObject(result, "deviceId", (double) deviceId);
        cJSON_AddNumberToObject(result, "wsPort", (double) wsPort);
        cJSON_AddBoolToObject(result, "mmsAvailable", mmsAvailable);
        cJSON_AddBoolToObject(result, "gooseAvailable", gooseAvailable);
    }
    cJSON_AddNullToObject(root, "error");

    return finish(root);
}

char*
ControlDispatcherJsonWriter_writeStopSuccess(const char* requestId, uint64_t deviceId) {
    cJSON* root = newEnvelope(requestId, "STOP_REPORTING", true);
    if (!root) return NULL;

    cJSON* result = cJSON_AddObjectToObject(root, "result");
    if (result) cJSON_AddNumberToObject(result, "deviceId", (double) deviceId);
    cJSON_AddNullToObject(root, "error");

    return finish(root);
}

char*
ControlDispatcherJsonWriter_writeScanStartSuccess(const char* requestId, uint64_t scanId) {
    cJSON* root = newEnvelope(requestId, "START_SCAN", true);
    if (!root) return NULL;

    cJSON* result = cJSON_AddObjectToObject(root, "result");
    if (result) cJSON_AddNumberToObject(result, "scanId", (double) scanId);
    cJSON_AddNullToObject(root, "error");

    return finish(root);
}

char*
ControlDispatcherJsonWriter_writeScanStopSuccess(const char* requestId, uint64_t scanId) {
    cJSON* root = newEnvelope(requestId, "STOP_SCAN", true);
    if (!root) return NULL;

    cJSON* result = cJSON_AddObjectToObject(root, "result");
    if (result) cJSON_AddNumberToObject(result, "scanId", (double) scanId);
    cJSON_AddNullToObject(root, "error");

    return finish(root);
}

char*
ControlDispatcherJsonWriter_writeError(const char* requestId, const char* action, const char* errorCode,
        const char* message, const char* stage, const char* detail) {
    cJSON* root = newEnvelope(requestId, action, false);
    if (!root) return NULL;

    cJSON_AddNullToObject(root, "result");

    cJSON* error = cJSON_AddObjectToObject(root, "error");
    if (error) {
        cJSON_AddStringToObject(error, "code", errorCode ? errorCode : "UNKNOWN_ERROR");
        cJSON_AddStringToObject(error, "message", message ? message : "");
        if (stage) cJSON_AddStringToObject(error, "stage", stage);
        if (detail) cJSON_AddStringToObject(error, "detail", detail);
    }

    return finish(root);
}
