#include <stdlib.h>
#include "features/ipc_dispatcher/data/ipc_dispatcher_conn_state_adapter.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_json_writer.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ring_buffer.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ws_server.h"

void
IpcDispatcherConnStateAdapter_handleConnStateChange(IpcDispatcherHandle handle, MmsReportClientConnState state) {
    if (!handle) return;

    char* json = IpcDispatcherJsonWriter_writeConnectionStatus(state);
    if (!json) return; /* no-op state (see writer's own doc comment) */

    IpcDispatcherRingBuffer_push(handle->ringBuffer, json); /* ownership -> ring buffer */
    IpcDispatcherWsServer_wake(handle->wsServer);
}
