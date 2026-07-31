#ifndef IPC_DISPATCHER_CONN_STATE_ADAPTER_H_
#define IPC_DISPATCHER_CONN_STATE_ADAPTER_H_

#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"
#include "features/mms_report_client/service/mms_report_client_api.h"

/*
 * Real logic behind IpcDispatcher_onConnStateChange (service/ipc_dispatcher_api.c
 * is a thin wrapper delegating here, matching ipc_dispatcher_mms_adapter.c/
 * ipc_dispatcher_goose_adapter.c's own delegation pattern). Builds the
 * CONNECTION_STATUS JSON (IpcDispatcherJsonWriter_writeConnectionStatus),
 * updates the ws server's retained copy, and enqueues it onto the ring
 * buffer - no-op for every state that writer doesn't build JSON for (see
 * that function's own doc comment). Fast, non-blocking, safe to call from
 * mms_report_client's reconnect-supervisor thread, same contract as the
 * report/GOOSE adapters.
 */
void
IpcDispatcherConnStateAdapter_handleConnStateChange(IpcDispatcherHandle handle, MmsReportClientConnState state);

#endif /* IPC_DISPATCHER_CONN_STATE_ADAPTER_H_ */
