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
        const DeviceManagerErrorDetail* detail, bool mmsAvailable, bool gooseAvailable) {
    if (err == DEVICE_MANAGER_OK) {
        return ControlDispatcherJsonWriter_writeStartSuccess(requestId, deviceId, wsPort, mmsAvailable,
                gooseAvailable);
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

static const char*
scanOrchestrationErrorToCode(ScanOrchestrationError err) {
    switch (err) {
        case SCAN_ORCHESTRATION_OK: return "OK";
        case SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case SCAN_ORCHESTRATION_ERR_DISPATCHER_START_FAILED: return "DISPATCHER_START_FAILED";
        case SCAN_ORCHESTRATION_ERR_THREAD_CREATE_FAILED: return "THREAD_CREATE_FAILED";
        case SCAN_ORCHESTRATION_ERR_DISCOVERY_CREATE_FAILED: return "DISCOVERY_CREATE_FAILED";
        case SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND: return "SCAN_NOT_FOUND";
        default: return "UNKNOWN_ERROR";
    }
}

static const char*
scanOrchestrationErrorToMessage(ScanOrchestrationError err) {
    switch (err) {
        case SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT: return "invalid argument (check interfaceId/mmsPort)";
        case SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY: return "out of memory";
        case SCAN_ORCHESTRATION_ERR_DISPATCHER_START_FAILED: return "scan_dispatcher websocket failed to bind";
        case SCAN_ORCHESTRATION_ERR_THREAD_CREATE_FAILED: return "failed to start the scan's worker thread";
        case SCAN_ORCHESTRATION_ERR_DISCOVERY_CREATE_FAILED: return "failed to create the underlying discovery handle";
        case SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND: return "unknown or already-stopped scanId";
        default: return "unknown scan_orchestration error";
    }
}

static char*
buildScanStartResponse(const char* requestId, ScanOrchestrationError err, uint64_t scanId) {
    if (err == SCAN_ORCHESTRATION_OK) {
        return ControlDispatcherJsonWriter_writeScanStartSuccess(requestId, scanId);
    }

    return ControlDispatcherJsonWriter_writeError(requestId, "START_SCAN", scanOrchestrationErrorToCode(err),
            scanOrchestrationErrorToMessage(err), NULL, NULL);
}

static char*
buildScanStopResponse(const char* requestId, ScanOrchestrationError err, uint64_t scanId) {
    if (err == SCAN_ORCHESTRATION_OK) {
        return ControlDispatcherJsonWriter_writeScanStopSuccess(requestId, scanId);
    }

    return ControlDispatcherJsonWriter_writeError(requestId, "STOP_SCAN", scanOrchestrationErrorToCode(err),
            scanOrchestrationErrorToMessage(err), NULL, NULL);
}

char*
ControlDispatcherUseCases_processRequest(const ControlRequest* request, DeviceManagerHandle deviceManager,
        ScanOrchestrationHandle scanOrchestration) {
    if (!request) return NULL;

    switch (request->type) {
        case CONTROL_REQ_START_REPORTING: {
            uint64_t deviceId = 0;
            uint16_t wsPort = 0;
            bool mmsAvailable = false;
            bool gooseAvailable = false;
            DeviceManagerErrorDetail detail;
            DeviceManagerError err = DeviceManager_startReporting(deviceManager, request->host, request->mmsPort,
                    request->iedName, request->interfaceId, request->sclFilePath, request->acseAuthPassword,
                    request->accessMode, request->lnCategoryFilter, &deviceId, &wsPort, &detail, &mmsAvailable,
                    &gooseAvailable);

            return buildStartResponse(request->requestId, err, deviceId, wsPort, &detail, mmsAvailable,
                    gooseAvailable);
        }

        case CONTROL_REQ_STOP_REPORTING: {
            /* Two mutually exclusive forms - see ControlRequest's own doc comment
             * (control_dispatcher_types.h) on the host != NULL discriminator. */
            if (request->host) {
                uint64_t resolvedDeviceId = 0;
                DeviceManagerError err = DeviceManager_stopReportingByAddress(deviceManager, request->host,
                        request->mmsPort, &resolvedDeviceId, NULL);
                return buildStopResponse(request->requestId, err, resolvedDeviceId);
            }

            DeviceManagerError err = DeviceManager_stopReporting(deviceManager, request->deviceId, NULL);
            return buildStopResponse(request->requestId, err, request->deviceId);
        }

        case CONTROL_REQ_START_SCAN: {
            /* acseAuthPassword left unauthenticated (NULL) for control-triggered
             * scans - not part of the original ask (interface + port), and
             * ScanRequest's own field is per-scan already if this needs
             * extending later. */
            ScanRequest scanRequest = {
                .interfaceId = request->interfaceId,
                .mmsPort = request->mmsPort,
                .sweepIntervalMs = request->sweepIntervalMs,
                .acseAuthPassword = NULL,
            };
            uint64_t scanId = 0;
            ScanOrchestrationError err = ScanOrchestration_startScan(scanOrchestration, &scanRequest, &scanId);
            return buildScanStartResponse(request->requestId, err, scanId);
        }

        default: { /* CONTROL_REQ_STOP_SCAN */
            ScanOrchestrationError err = ScanOrchestration_stopScan(scanOrchestration, request->scanId);
            return buildScanStopResponse(request->requestId, err, request->scanId);
        }
    }
}
