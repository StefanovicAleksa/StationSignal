#ifndef IPC_DISPATCHER_GOOSE_ADAPTER_H_
#define IPC_DISPATCHER_GOOSE_ADAPTER_H_

#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"
#include "features/goose_subscriber/service/goose_subscriber_api.h"

/*
 * Structural mirror of ipc_dispatcher_mms_adapter.h for GOOSE. Always calls
 * GooseSubscription_destroyRecord(record) before returning. Safe to call
 * from goose_subscriber's GooseReceiver reception thread.
 */
void
IpcDispatcherGooseAdapter_handleRecord(IpcDispatcherHandle handle, const GooseSubscriberRecord* record);

#endif /* IPC_DISPATCHER_GOOSE_ADAPTER_H_ */
