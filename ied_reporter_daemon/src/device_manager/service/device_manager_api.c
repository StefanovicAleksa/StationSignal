#include <stdlib.h>
#include <string.h>
#include "device_manager/service/device_manager_api.h"
#include "device_manager/data/device_manager_registry.h"
#include "device_manager/domain/device_manager_bootstrap_policy.h"

void
DeviceManagerConfig_defaults(DeviceManagerConfig* config) {
    if (!config) return;

    config->wsPortRangeStart = 9000;
    config->wsPortRangeEnd = 9999;
}

DeviceManagerHandle
DeviceManager_create(const DeviceManagerConfig* config, DeviceManagerError* outError) {
    DeviceManagerHandle handle = calloc(1, sizeof(struct sDeviceManagerHandle));
    if (!handle) {
        if (outError) *outError = DEVICE_MANAGER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config) {
        handle->config = *config;
    } else {
        DeviceManagerConfig_defaults(&handle->config);
    }

    if (handle->config.wsPortRangeStart > handle->config.wsPortRangeEnd) {
        free(handle);
        if (outError) *outError = DEVICE_MANAGER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    handle->registry = DeviceManagerRegistry_create(handle->config.wsPortRangeStart, handle->config.wsPortRangeEnd);
    if (!handle->registry) {
        free(handle);
        if (outError) *outError = DEVICE_MANAGER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (outError) *outError = DEVICE_MANAGER_OK;
    return handle;
}

static bool
isEmpty(const char* s) {
    return !s || !s[0];
}

static void
clearDetail(DeviceManagerErrorDetail* outDetail) {
    if (!outDetail) return;
    outDetail->orchestrationError = ORCHESTRATION_OK;
    memset(&outDetail->orchestrationDetail, 0, sizeof(outDetail->orchestrationDetail));
}

DeviceManagerError
DeviceManager_startReporting(DeviceManagerHandle handle, const char* host, int mmsPort,
        const char* iedName, const char* interfaceId, const char* sclFilePath,
        const char* acseAuthPassword, AccessMode accessMode,
        uint64_t* outDeviceId, uint16_t* outWsPort, DeviceManagerErrorDetail* outDetail) {
    clearDetail(outDetail);

    if (!handle || isEmpty(host) || isEmpty(interfaceId) || !outDeviceId || !outWsPort) {
        return DEVICE_MANAGER_ERR_INVALID_ARGUMENT;
    }

    const char* effectiveSclFilePath = isEmpty(sclFilePath) ? NULL : sclFilePath;
    const char* effectiveIedName = isEmpty(iedName) ? NULL : iedName;
    const char* effectiveAcseAuthPassword = isEmpty(acseAuthPassword) ? NULL : acseAuthPassword;

    /* Per this feature's own explicit contract: iedName is mandatory whenever
     * sclFilePath is given (Orchestration_runFromLocalFile would still
     * technically auto-detect it, same as Orchestration_run does, but this
     * feature makes it required instead). */
    if (effectiveSclFilePath && !effectiveIedName) return DEVICE_MANAGER_ERR_INVALID_ARGUMENT;

    uint64_t deviceId;
    uint16_t wsPort;
    const char* ownedAcseAuthPassword = NULL;
    DeviceManagerError reserveErr = DeviceManagerRegistry_reserve(handle->registry, host, mmsPort,
            effectiveAcseAuthPassword, &deviceId, &wsPort, &ownedAcseAuthPassword);
    if (reserveErr != DEVICE_MANAGER_OK) return reserveErr;

    /* --- slow part, NO registry lock held: real network I/O --- */

    OrchestrationConfig orchestrationConfig;
    OrchestrationConfig_defaults(&orchestrationConfig);
    orchestrationConfig.ipcDispatcherConfig.port = wsPort;
    /* Registry's own owned copy (borrowed here) - stays valid for the whole
     * OrchestrationHandle lifetime, unlike the caller's own
     * acseAuthPassword buffer which may not outlive this one call (e.g. a
     * parsed control-plane message). See device_manager_registry.h's own
     * top comment. */
    orchestrationConfig.bootstrapConfig.acseAuthPassword = ownedAcseAuthPassword;
    orchestrationConfig.reportClientConfig.acseAuthPassword = ownedAcseAuthPassword;

    OrchestrationError createError;
    OrchestrationHandle orchestrationHandle = Orchestration_create(&orchestrationConfig, &createError);
    if (!orchestrationHandle) {
        DeviceManagerRegistry_rollback(handle->registry, deviceId);
        if (outDetail) outDetail->orchestrationError = createError;
        return DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED;
    }

    /* Surfaces a rejected MMS report-client connection (e.g. wrong/missing
     * ACSE password on the actual report association, discovered only after
     * SCL bootstrap already succeeded) as a CONNECTION_STATUS push on this
     * device's own stream - see orchestration_api.h's own doc comment on this
     * wrapper. Must be called before DeviceManagerBootstrapPolicy_run/
     * Orchestration_run, same as every Orchestration_set*Callback. */
    Orchestration_wireConnStatusToIpcDispatcher(orchestrationHandle);

    OrchestrationErrorDetail detail;
    memset(&detail, 0, sizeof(detail));
    OrchestrationError runError = DeviceManagerBootstrapPolicy_run(orchestrationHandle, host, mmsPort,
            effectiveIedName, interfaceId, effectiveSclFilePath, effectiveAcseAuthPassword, accessMode, &detail);

    if (runError != ORCHESTRATION_OK) {
        Orchestration_destroy(orchestrationHandle);
        DeviceManagerRegistry_rollback(handle->registry, deviceId);
        if (outDetail) {
            outDetail->orchestrationError = runError;
            outDetail->orchestrationDetail = detail;
        }
        return DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED;
    }

    /* --- back under a short lock to finalize --- */

    DeviceManagerRegistry_finalize(handle->registry, deviceId, orchestrationHandle);

    *outDeviceId = deviceId;
    *outWsPort = wsPort;
    return DEVICE_MANAGER_OK;
}

DeviceManagerError
DeviceManager_stopReporting(DeviceManagerHandle handle, uint64_t deviceId, DeviceManagerErrorDetail* outDetail) {
    clearDetail(outDetail);

    if (!handle) return DEVICE_MANAGER_ERR_INVALID_ARGUMENT;

    OrchestrationHandle orchestrationHandle = NULL;
    uint16_t wsPort = 0;
    char* ownedHost = NULL;
    char* ownedAcseAuthPassword = NULL;
    DeviceManagerError removeErr = DeviceManagerRegistry_removeIfRunning(handle->registry, deviceId,
            &orchestrationHandle, &wsPort, &ownedHost, &ownedAcseAuthPassword);
    if (removeErr != DEVICE_MANAGER_OK) return removeErr;

    /* --- slow part, NO registry lock held --- */

    Orchestration_stop(orchestrationHandle);
    Orchestration_destroy(orchestrationHandle);

    /* Only safe to free now that Orchestration_destroy has actually run -
     * OrchestrationConfig's acseAuthPassword fields stay borrowed for the
     * handle's whole lifetime (see device_manager_registry.h's own top
     * comment). */
    free(ownedHost);
    free(ownedAcseAuthPassword);

    DeviceManagerRegistry_freePort(handle->registry, wsPort);

    return DEVICE_MANAGER_OK;
}

void
DeviceManager_destroy(DeviceManagerHandle handle) {
    if (!handle) return;

    uint64_t deviceId;
    while ((deviceId = DeviceManagerRegistry_anyRunningDeviceId(handle->registry)) != 0) {
        DeviceManager_stopReporting(handle, deviceId, NULL);
    }

    if (handle->registry) {
        DeviceManagerRegistry_destroy(handle->registry);
        handle->registry = NULL;
    }
    free(handle);
}
