#include <stddef.h>
#include "features/control_dispatcher/domain/control_dispatcher_usecases.h"
#include "features/control_dispatcher/data/control_dispatcher_json_writer.h"
#include "orchestration/utils/orchestration_utils.h"

static const char*
deviceManagerErrorToCode(DeviceManagerError err) {
    switch (err) {
        case DEVICE_MANAGER_OK: return "OK";
        case DEVICE_MANAGER_ERR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case DEVICE_MANAGER_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case DEVICE_MANAGER_ERR_PORT_EXHAUSTED: return "PORT_EXHAUSTED";
        case DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING: return "HOST_ALREADY_RUNNING";
        case DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED: return "ORCHESTRATION_FAILED";
        case DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND: return "DEVICE_NOT_FOUND";
        case DEVICE_MANAGER_ERR_START_IN_PROGRESS: return "START_IN_PROGRESS";
        default: return "UNKNOWN_ERROR";
    }
}

static const char*
deviceManagerErrorToMessage(DeviceManagerError err) {
    switch (err) {
        case DEVICE_MANAGER_ERR_INVALID_ARGUMENT: return "invalid argument";
        case DEVICE_MANAGER_ERR_OUT_OF_MEMORY: return "out of memory";
        case DEVICE_MANAGER_ERR_PORT_EXHAUSTED: return "no free websocket port left in the configured range";
        case DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING: return "this host/mmsPort is already running or starting";
        case DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED: return "orchestration failed";
        case DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND: return "unknown or already-stopped deviceId";
        case DEVICE_MANAGER_ERR_START_IN_PROGRESS:
            return "device is still starting on another request - retry shortly";
        default: return "unknown device_manager error";
    }
}

static char*
buildStartResponse(const char* requestId, DeviceManagerError err, uint64_t deviceId, uint16_t wsPort,
        const DeviceManagerErrorDetail* detail) {
    if (err == DEVICE_MANAGER_OK) {
        return ControlDispatcherJsonWriter_writeStartSuccess(requestId, deviceId, wsPort);
    }

    const char* stage = NULL;
    const char* detailStr = NULL;
    if (err == DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED && detail) {
        stage = OrchestrationUtils_stageToString(detail->orchestrationDetail.stage);
        if (detail->orchestrationDetail.stage == ORCHESTRATION_STAGE_BOOTSTRAP) {
            detailStr = OrchestrationUtils_candidateStatusToString(detail->orchestrationDetail.lastCandidateStatus);
        }
    }

    return ControlDispatcherJsonWriter_writeError(requestId, "START_REPORTING", deviceManagerErrorToCode(err),
            deviceManagerErrorToMessage(err), stage, detailStr);
}

static char*
buildStopResponse(const char* requestId, DeviceManagerError err, uint64_t deviceId) {
    if (err == DEVICE_MANAGER_OK) {
        return ControlDispatcherJsonWriter_writeStopSuccess(requestId, deviceId);
    }

    return ControlDispatcherJsonWriter_writeError(requestId, "STOP_REPORTING", deviceManagerErrorToCode(err),
            deviceManagerErrorToMessage(err), NULL, NULL);
}

char*
ControlDispatcherUseCases_processRequest(const ControlRequest* request, DeviceManagerHandle deviceManager) {
    if (!request) return NULL;

    if (request->type == CONTROL_REQ_START_REPORTING) {
        uint64_t deviceId = 0;
        uint16_t wsPort = 0;
        DeviceManagerErrorDetail detail;
        DeviceManagerError err = DeviceManager_startReporting(deviceManager, request->host, request->mmsPort,
                request->iedName, request->interfaceId, request->sclFilePath, request->acseAuthPassword,
                request->accessMode, &deviceId, &wsPort, &detail);

        return buildStartResponse(request->requestId, err, deviceId, wsPort, &detail);
    }

    DeviceManagerError err = DeviceManager_stopReporting(deviceManager, request->deviceId, NULL);
    return buildStopResponse(request->requestId, err, request->deviceId);
}
