#include <stdlib.h>
#include "features/scan_dispatcher/service/scan_dispatcher_api.h"
#include "features/scan_dispatcher/data/scan_dispatcher_ring_buffer.h"
#include "features/scan_dispatcher/data/scan_dispatcher_ws_server.h"
#include "features/scan_dispatcher/data/scan_dispatcher_adapter.h"

void
ScanDispatcherConfig_defaults(ScanDispatcherConfig* config) {
    if (!config) return;

    config->port = 8766;
    config->ringBufferCapacity = 256;
    config->maxConnections = 16;
}

ScanDispatcherHandle
ScanDispatcher_create(const ScanDispatcherConfig* config, ScanDispatcherError* outError) {
    ScanDispatcherHandle handle = calloc(1, sizeof(struct sScanDispatcherHandle));
    if (!handle) {
        if (outError) *outError = SCAN_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config) {
        handle->config = *config;
    } else {
        ScanDispatcherConfig_defaults(&handle->config);
    }

    if (handle->config.ringBufferCapacity <= 0 || handle->config.maxConnections <= 0) {
        free(handle);
        if (outError) *outError = SCAN_DISPATCHER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    handle->ringBuffer = ScanDispatcherRingBuffer_create(handle->config.ringBufferCapacity);
    if (!handle->ringBuffer) {
        free(handle);
        if (outError) *outError = SCAN_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (outError) *outError = SCAN_DISPATCHER_OK;
    return handle;
}

ScanDispatcherError
ScanDispatcher_start(ScanDispatcherHandle handle) {
    if (!handle) return SCAN_DISPATCHER_ERR_INVALID_ARGUMENT;
    if (handle->running) return SCAN_DISPATCHER_ERR_ALREADY_RUNNING;

    ScanDispatcherError err;
    handle->wsServer = ScanDispatcherWsServer_create(
            handle->config.port, handle->config.maxConnections, handle->ringBuffer, &err);
    if (!handle->wsServer) return err;

    err = ScanDispatcherWsServer_start(handle->wsServer);
    if (err != SCAN_DISPATCHER_OK) {
        ScanDispatcherWsServer_destroy(handle->wsServer);
        handle->wsServer = NULL;
        return err;
    }

    handle->running = true;
    return SCAN_DISPATCHER_OK;
}

void
ScanDispatcher_stop(ScanDispatcherHandle handle) {
    if (!handle) return;

    /* Fully tears down the ws server (thread + lws context), not just the
     * thread - same reasoning as IpcDispatcher_stop's own comment: a clean
     * restart via ScanDispatcher_start requires releasing the port here. */
    if (handle->wsServer) {
        ScanDispatcherWsServer_destroy(handle->wsServer);
        handle->wsServer = NULL;
    }
    handle->running = false;
}

void
ScanDispatcher_destroy(ScanDispatcherHandle handle) {
    if (!handle) return;

    ScanDispatcher_stop(handle); /* idempotent - safe even if never started */
    if (handle->ringBuffer) {
        ScanDispatcherRingBuffer_destroy(handle->ringBuffer);
        handle->ringBuffer = NULL;
    }
    free(handle);
}

void
ScanDispatcher_publishDeviceFound(ScanDispatcherHandle handle, uint64_t scanId, const char* host, int mmsPort,
        bool authRequired) {
    ScanDispatcherAdapter_publishDeviceFound(handle, scanId, host, mmsPort, authRequired);
}
