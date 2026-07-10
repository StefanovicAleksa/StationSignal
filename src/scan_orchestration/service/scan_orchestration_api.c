#include <stdlib.h>
#include "scan_orchestration/service/scan_orchestration_api.h"
#include "scan_orchestration/data/scan_orchestration_registry.h"
#include "scan_orchestration/data/scan_orchestration_worker.h"
#include "features/scan_dispatcher/service/scan_dispatcher_api.h"

void
ScanOrchestrationConfig_defaults(ScanOrchestrationConfig* config) {
    if (!config) return;

    ScanDispatcherConfig_defaults(&config->scanDispatcherConfig);
    config->defaultSweepIntervalMs = 10000;
    IedDiscoveryConfig_defaults(&config->discoveryConfigTemplate);
}

ScanOrchestrationHandle
ScanOrchestration_create(const ScanOrchestrationConfig* config, ScanOrchestrationError* outError) {
    ScanOrchestrationHandle handle = calloc(1, sizeof(struct sScanOrchestrationHandle));
    if (!handle) {
        if (outError) *outError = SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config) {
        handle->config = *config;
    } else {
        ScanOrchestrationConfig_defaults(&handle->config);
    }

    handle->registry = ScanOrchestrationRegistry_create();
    if (!handle->registry) {
        free(handle);
        if (outError) *outError = SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    ScanDispatcherError dispatcherErr;
    handle->scanDispatcher = ScanDispatcher_create(&handle->config.scanDispatcherConfig, &dispatcherErr);
    if (!handle->scanDispatcher) {
        ScanOrchestrationRegistry_destroy(handle->registry);
        free(handle);
        if (outError) *outError = SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (outError) *outError = SCAN_ORCHESTRATION_OK;
    return handle;
}

void
ScanOrchestration_setDeviceFoundCallback(ScanOrchestrationHandle handle,
        ScanOrchestrationDeviceFoundCallback callback, void* userParam) {
    if (!handle) return;

    handle->foundCallback = callback;
    handle->foundCallbackParam = userParam;
}

ScanOrchestrationError
ScanOrchestration_startScan(ScanOrchestrationHandle handle, const ScanRequest* request, uint64_t* outScanId) {
    if (!handle || !request) return SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT;

    uint64_t scanId = ScanOrchestrationRegistry_nextScanId(handle->registry);

    ScanOrchestrationError err;
    ScanOrchestrationWorker worker = ScanOrchestrationWorker_create(scanId, request,
            &handle->config.discoveryConfigTemplate, handle->config.defaultSweepIntervalMs,
            handle->scanDispatcher, handle->foundCallback, handle->foundCallbackParam, &err);
    if (!worker) return err;

    err = ScanOrchestrationRegistry_addAndMaybeStartDispatcher(handle->registry, handle->scanDispatcher, worker);
    if (err != SCAN_ORCHESTRATION_OK) {
        ScanOrchestrationWorker_destroy(worker);
        return err;
    }

    err = ScanOrchestrationWorker_start(worker);
    if (err != SCAN_ORCHESTRATION_OK) {
        /* Roll back the registration made above - mirrors this codebase's
         * existing fail-hard rollback convention (see orchestration's own). */
        ScanOrchestrationWorker removed = NULL;
        bool nowEmpty = false;
        ScanOrchestrationRegistry_remove(handle->registry, scanId, &removed, &nowEmpty);
        if (nowEmpty) ScanDispatcher_stop(handle->scanDispatcher);
        ScanOrchestrationWorker_destroy(worker);
        return err;
    }

    if (outScanId) *outScanId = scanId;
    return SCAN_ORCHESTRATION_OK;
}

ScanOrchestrationError
ScanOrchestration_stopScan(ScanOrchestrationHandle handle, uint64_t scanId) {
    if (!handle) return SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT;

    ScanOrchestrationWorker worker = NULL;
    bool nowEmpty = false;
    if (!ScanOrchestrationRegistry_remove(handle->registry, scanId, &worker, &nowEmpty)) {
        return SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND;
    }

    /* Implies _stop() - may block for the duration of an in-flight sweep,
     * see ScanOrchestrationWorker_stop's own doc comment. Deliberately
     * outside the registry lock (already released by _remove above) so
     * other concurrent scans' start/stop calls are never blocked by this. */
    ScanOrchestrationWorker_destroy(worker);

    if (nowEmpty) ScanDispatcher_stop(handle->scanDispatcher);

    return SCAN_ORCHESTRATION_OK;
}

ScanOrchestrationError
ScanOrchestration_snapshotDiscoveredHosts(ScanOrchestrationHandle handle, uint64_t scanId,
        char*** outHosts, int* outCount) {
    if (!handle || !outHosts || !outCount) return SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT;

    ScanOrchestrationWorker worker = ScanOrchestrationRegistry_find(handle->registry, scanId);
    if (!worker) return SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND;

    *outHosts = ScanOrchestrationWorker_snapshotHosts(worker, outCount);
    return SCAN_ORCHESTRATION_OK;
}

void
ScanOrchestration_freeDiscoveredHostsSnapshot(char** hosts, int count) {
    ScanOrchestrationWorker_freeSnapshot(hosts, count);
}

void
ScanOrchestration_destroy(ScanOrchestrationHandle handle) {
    if (!handle) return;

    uint64_t scanId;
    while ((scanId = ScanOrchestrationRegistry_anyActiveScanId(handle->registry)) != 0) {
        ScanOrchestration_stopScan(handle, scanId);
    }

    if (handle->registry) {
        ScanOrchestrationRegistry_destroy(handle->registry);
        handle->registry = NULL;
    }
    if (handle->scanDispatcher) {
        ScanDispatcher_destroy(handle->scanDispatcher);
        handle->scanDispatcher = NULL;
    }
    free(handle);
}
