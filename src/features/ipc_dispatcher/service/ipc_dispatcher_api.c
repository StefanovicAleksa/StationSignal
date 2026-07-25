#include <stdlib.h>
#include "features/ipc_dispatcher/service/ipc_dispatcher_api.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ring_buffer.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ws_server.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_mms_adapter.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_goose_adapter.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_conn_state_adapter.h"

void
IpcDispatcherConfig_defaults(IpcDispatcherConfig* config) {
    if (!config) return;

    config->port = 8765;
    config->ringBufferCapacity = 256;
    config->maxConnections = 16;
}

IpcDispatcherHandle
IpcDispatcher_create(const IpcDispatcherConfig* config, IpcDispatcherError* outError) {
    IpcDispatcherHandle handle = calloc(1, sizeof(struct sIpcDispatcherHandle));
    if (!handle) {
        if (outError) *outError = IPC_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config) {
        handle->config = *config;
    } else {
        IpcDispatcherConfig_defaults(&handle->config);
    }

    if (handle->config.ringBufferCapacity <= 0 || handle->config.maxConnections <= 0) {
        free(handle);
        if (outError) *outError = IPC_DISPATCHER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    handle->ringBuffer = IpcDispatcherRingBuffer_create(handle->config.ringBufferCapacity);
    if (!handle->ringBuffer) {
        free(handle);
        if (outError) *outError = IPC_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (outError) *outError = IPC_DISPATCHER_OK;
    return handle;
}

IpcDispatcherError
IpcDispatcher_start(IpcDispatcherHandle handle) {
    if (!handle) return IPC_DISPATCHER_ERR_INVALID_ARGUMENT;
    if (handle->running) return IPC_DISPATCHER_ERR_ALREADY_RUNNING;

    IpcDispatcherError err;
    handle->wsServer = IpcDispatcherWsServer_create(
            handle->config.port, handle->config.maxConnections, handle->ringBuffer, &err);
    if (!handle->wsServer) return err;

    err = IpcDispatcherWsServer_start(handle->wsServer);
    if (err != IPC_DISPATCHER_OK) {
        IpcDispatcherWsServer_destroy(handle->wsServer);
        handle->wsServer = NULL;
        return err;
    }

    handle->running = true;
    return IPC_DISPATCHER_OK;
}

void
IpcDispatcher_stop(IpcDispatcherHandle handle) {
    if (!handle) return;

    /* Fully tears down the ws server (thread + lws context), not just the
     * thread - unlike goose_subscriber's GooseReceiver, an lws context's
     * listening socket has no "stop servicing, keep the bind" mode, so a
     * clean restart via IpcDispatcher_start requires releasing the port
     * here rather than leaving a stopped-but-still-bound server behind. */
    if (handle->wsServer) {
        IpcDispatcherWsServer_destroy(handle->wsServer);
        handle->wsServer = NULL;
    }
    handle->running = false;
}

void
IpcDispatcher_destroy(IpcDispatcherHandle handle) {
    if (!handle) return;

    IpcDispatcher_stop(handle); /* idempotent - safe even if never started */
    if (handle->ringBuffer) {
        IpcDispatcherRingBuffer_destroy(handle->ringBuffer);
        handle->ringBuffer = NULL;
    }
    free(handle);
}

void
IpcDispatcher_onMmsReport(void* userParam, const MmsReportRecord* record) {
    IpcDispatcherMmsAdapter_handleReport((IpcDispatcherHandle) userParam, record);
}

void
IpcDispatcher_onGooseRecord(void* userParam, const GooseSubscriberRecord* record) {
    IpcDispatcherGooseAdapter_handleRecord((IpcDispatcherHandle) userParam, record);
}

void
IpcDispatcher_onConnStateChange(void* userParam, MmsReportClientConnState state) {
    IpcDispatcherConnStateAdapter_handleConnStateChange((IpcDispatcherHandle) userParam, state);
}
