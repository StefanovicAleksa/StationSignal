#include <stdlib.h>
#include "features/control_dispatcher/service/control_dispatcher_api.h"
#include "features/control_dispatcher/data/control_dispatcher_ring_buffer.h"
#include "features/control_dispatcher/data/control_dispatcher_request_queue.h"
#include "features/control_dispatcher/data/control_dispatcher_ws_server.h"
#include "features/control_dispatcher/data/control_dispatcher_worker.h"
#include "features/control_dispatcher/data/control_dispatcher_json_writer.h"

void
ControlDispatcherConfig_defaults(ControlDispatcherConfig* config) {
    if (!config) return;

    config->port = 8767;
    config->ringBufferCapacity = 256;
    config->maxConnections = 16;
    config->requestQueueCapacity = 64;
}

ControlDispatcherHandle
ControlDispatcher_create(const ControlDispatcherConfig* config, DeviceManagerHandle deviceManager,
        ScanOrchestrationHandle scanOrchestration, ControlDispatcherError* outError) {
    if (!deviceManager || !scanOrchestration) {
        if (outError) *outError = CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    ControlDispatcherHandle handle = calloc(1, sizeof(struct sControlDispatcherHandle));
    if (!handle) {
        if (outError) *outError = CONTROL_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config) {
        handle->config = *config;
    } else {
        ControlDispatcherConfig_defaults(&handle->config);
    }

    if (handle->config.ringBufferCapacity <= 0 || handle->config.maxConnections <= 0
            || handle->config.requestQueueCapacity <= 0) {
        free(handle);
        if (outError) *outError = CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    handle->deviceManager = deviceManager;
    handle->scanOrchestration = scanOrchestration;

    handle->ringBuffer = (struct sControlDispatcherRingBuffer*)
            ControlDispatcherRingBuffer_create(handle->config.ringBufferCapacity);
    if (!handle->ringBuffer) {
        free(handle);
        if (outError) *outError = CONTROL_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    handle->requestQueue = (struct sControlDispatcherRequestQueue*)
            ControlDispatcherRequestQueue_create(handle->config.requestQueueCapacity);
    if (!handle->requestQueue) {
        ControlDispatcherRingBuffer_destroy((ControlDispatcherRingBuffer) handle->ringBuffer);
        free(handle);
        if (outError) *outError = CONTROL_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (outError) *outError = CONTROL_DISPATCHER_OK;
    return handle;
}

static void
onRequestQueuedTrampoline(void* param) {
    ControlDispatcherWorker_notify((ControlDispatcherWorker) param);
}

ControlDispatcherError
ControlDispatcher_start(ControlDispatcherHandle handle) {
    if (!handle) return CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT;
    if (handle->running) return CONTROL_DISPATCHER_ERR_ALREADY_RUNNING;

    ControlDispatcherError err;
    /* Bind first, deliberately, so a port conflict fails fast before ever
     * spinning up the worker thread - same "fail fast before the rest"
     * reasoning ipc_dispatcher/orchestration already apply to their own
     * first stage. Callback wired in below, once the worker (which needs
     * this already-created wsServer to wake) exists. */
    ControlDispatcherWsServer wsServer = ControlDispatcherWsServer_create(handle->config.port,
            handle->config.maxConnections, (ControlDispatcherRingBuffer) handle->ringBuffer,
            (ControlDispatcherRequestQueue) handle->requestQueue, NULL, NULL, &err);
    if (!wsServer) return err;

    ControlDispatcherWorker worker = ControlDispatcherWorker_create((ControlDispatcherRequestQueue) handle->requestQueue,
            (ControlDispatcherRingBuffer) handle->ringBuffer, wsServer, handle->deviceManager,
            handle->scanOrchestration, &err);
    if (!worker) {
        ControlDispatcherWsServer_destroy(wsServer);
        return err;
    }

    ControlDispatcherWsServer_setRequestQueuedCallback(wsServer, onRequestQueuedTrampoline, worker);

    err = ControlDispatcherWsServer_start(wsServer);
    if (err != CONTROL_DISPATCHER_OK) {
        ControlDispatcherWorker_destroy(worker);
        ControlDispatcherWsServer_destroy(wsServer);
        return err;
    }

    err = ControlDispatcherWorker_start(worker);
    if (err != CONTROL_DISPATCHER_OK) {
        ControlDispatcherWorker_destroy(worker);
        ControlDispatcherWsServer_destroy(wsServer);
        return err;
    }

    handle->wsServer = (struct sControlDispatcherWsServer*) wsServer;
    handle->worker = (struct sControlDispatcherWorker*) worker;
    handle->running = true;
    return CONTROL_DISPATCHER_OK;
}

void
ControlDispatcher_stop(ControlDispatcherHandle handle) {
    if (!handle) return;

    /* Worker first (stop accepting/finishing processing), then the ws
     * server (fully tears down the lws context - same reasoning as
     * ScanDispatcher_stop's own comment: a clean restart via
     * ControlDispatcher_start requires releasing the port here). Nothing
     * can push a NEW request into the (about-to-be-destroyed) worker once
     * the ws server itself is gone, but stopping the worker first means no
     * in-flight processing is racing the ws server's own teardown either. */
    if (handle->worker) {
        ControlDispatcherWorker_destroy((ControlDispatcherWorker) handle->worker);
        handle->worker = NULL;
    }
    if (handle->wsServer) {
        ControlDispatcherWsServer_destroy((ControlDispatcherWsServer) handle->wsServer);
        handle->wsServer = NULL;
    }
    handle->running = false;
}

void
ControlDispatcher_destroy(ControlDispatcherHandle handle) {
    if (!handle) return;

    ControlDispatcher_stop(handle); /* idempotent - safe even if never started */
    if (handle->ringBuffer) {
        ControlDispatcherRingBuffer_destroy((ControlDispatcherRingBuffer) handle->ringBuffer);
        handle->ringBuffer = NULL;
    }
    if (handle->requestQueue) {
        ControlDispatcherRequestQueue_destroy((ControlDispatcherRequestQueue) handle->requestQueue);
        handle->requestQueue = NULL;
    }
    free(handle);
}
