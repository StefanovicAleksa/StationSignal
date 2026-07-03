#ifndef IPC_DISPATCHER_MMS_ADAPTER_H_
#define IPC_DISPATCHER_MMS_ADAPTER_H_

#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"
#include "features/mms_report_client/service/mms_report_client_api.h"

/*
 * Real logic behind IpcDispatcher_onMmsReport (service/ipc_dispatcher_api.c
 * is a thin wrapper delegating here, matching goose_subscriber_api.c's own
 * delegation into data/goose_subscriber_frame_adapter.c). Extracts, pairs
 * quality, serializes to JSON, and enqueues onto the internal ring buffer -
 * fast, non-blocking, never touches a struct lws* directly (only the
 * thread-safe IpcDispatcherWsServer_wake) - safe to call from
 * mms_report_client's reconnect-supervisor thread. ALWAYS calls
 * MmsReportClient_destroyReportRecord(record) before returning - takes
 * ownership per that feature's callback contract.
 */
void
IpcDispatcherMmsAdapter_handleReport(IpcDispatcherHandle handle, const MmsReportRecord* record);

#endif /* IPC_DISPATCHER_MMS_ADAPTER_H_ */
