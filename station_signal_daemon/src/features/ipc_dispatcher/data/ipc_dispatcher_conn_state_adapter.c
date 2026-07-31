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

    /* Retained first (copies internally) so a client that connects between this push and the
     * ring buffer's next drain still sees the current status - see
     * IpcDispatcherWsServer_setRetainedConnStatus's own doc comment on why connection status
     * needs different semantics than the report/GOOSE change-stream. */
    IpcDispatcherWsServer_setRetainedConnStatus(handle->wsServer, json);
    IpcDispatcherRingBuffer_push(handle->ringBuffer, json); /* ownership -> ring buffer */
    IpcDispatcherWsServer_wake(handle->wsServer);
}
