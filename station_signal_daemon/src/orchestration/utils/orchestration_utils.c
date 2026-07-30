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
        case ORCHESTRATION_ERR_IED_NAME_RESOLUTION_FAILED: return "IED name auto-detection failed (zero or multiple <IED> elements)";
        case ORCHESTRATION_ERR_MODEL_LOAD_FAILED: return "IED model load failed";
        case ORCHESTRATION_ERR_REPORT_CLIENT_FAILED: return "MMS report client start failed";
        case ORCHESTRATION_ERR_GOOSE_SUBSCRIBER_FAILED: return "GOOSE subscriber start failed";
        case ORCHESTRATION_ERR_NO_CAPABILITIES: return "device has neither MMS report control blocks nor GOOSE control blocks";
        default: return "unknown orchestration error";
    }
}

const char*
OrchestrationUtils_stageToString(OrchestrationStage stage) {
    switch (stage) {
        case ORCHESTRATION_STAGE_NONE: return "none";
        case ORCHESTRATION_STAGE_IPC_DISPATCHER_START: return "ipc_dispatcher start";
        case ORCHESTRATION_STAGE_BOOTSTRAP: return "SCL bootstrap";
        case ORCHESTRATION_STAGE_STAGING: return "SCL staging";
        case ORCHESTRATION_STAGE_IED_NAME_RESOLUTION: return "IED name resolution";
        case ORCHESTRATION_STAGE_MODEL_LOAD: return "IED model load";
        case ORCHESTRATION_STAGE_ONLINE_DISCOVERY: return "online discovery";
        case ORCHESTRATION_STAGE_REPORT_CLIENT_START: return "MMS report client start";
        case ORCHESTRATION_STAGE_GOOSE_SUBSCRIBER_START: return "GOOSE subscriber start";
        case ORCHESTRATION_STAGE_NO_CAPABILITIES: return "no capabilities";
        default: return "unknown orchestration stage";
    }
}

const char*
OrchestrationUtils_candidateStatusToString(SclBootstrapCandidateStatus status) {
    switch (status) {
        case SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER: return "no MMS server (TCP never connected)";
        case SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED: return "MMS association/browse/download failed";
        case SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND:
            return "associated fine, but no SCL file (.icd/.cid/.scd/.ssd/.sed) found in its file directory";
        case SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED: return "access denied (auth required/rejected)";
        case SCL_BOOTSTRAP_CANDIDATE_DOWNLOAD_FAILED: return "SCL file found but download failed";
        case SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED: return "file retrieved (unexpected here)";
        default: return "unknown";
    }
}
