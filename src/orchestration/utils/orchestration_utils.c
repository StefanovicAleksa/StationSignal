#include <stdlib.h>
#include <string.h>
#include "orchestration/utils/orchestration_utils.h"

char*
OrchestrationUtils_safeStringDup(const char* s) {
    if (!s) return NULL;

    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

const char*
OrchestrationUtils_errorToString(OrchestrationError err) {
    switch (err) {
        case ORCHESTRATION_OK: return "OK";
        case ORCHESTRATION_ERR_INVALID_ARGUMENT: return "invalid argument";
        case ORCHESTRATION_ERR_OUT_OF_MEMORY: return "out of memory";
        case ORCHESTRATION_ERR_IPC_DISPATCHER_FAILED: return "ipc_dispatcher websocket bind/start failed";
        case ORCHESTRATION_ERR_BOOTSTRAP_FAILED: return "SCL bootstrap failed";
        case ORCHESTRATION_ERR_STAGING_FAILED: return "temp-file staging failed";
        case ORCHESTRATION_ERR_MODEL_LOAD_FAILED: return "IED model load failed";
        case ORCHESTRATION_ERR_REPORT_CLIENT_FAILED: return "MMS report client start failed";
        case ORCHESTRATION_ERR_GOOSE_SUBSCRIBER_FAILED: return "GOOSE subscriber start failed";
        default: return "unknown orchestration error";
    }
}
