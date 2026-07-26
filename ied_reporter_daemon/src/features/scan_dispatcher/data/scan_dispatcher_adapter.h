#ifndef SCAN_DISPATCHER_ADAPTER_H_
#define SCAN_DISPATCHER_ADAPTER_H_

#include "features/scan_dispatcher/domain/scan_dispatcher_types.h"

/*
 * Real logic behind ScanDispatcher_publishDeviceFound (service/
 * scan_dispatcher_api.c is a thin wrapper delegating here, matching
 * ipc_dispatcher_api.c's own delegation into data/ipc_dispatcher_mms_adapter.c).
 * Assembles the event, serializes to JSON, enqueues onto the internal ring
 * buffer, and wakes the service thread - fast, non-blocking, never touches a
 * struct lws* directly - safe to call from any scan's own worker thread.
 * No-op if handle is NULL or not currently running.
 */
void
ScanDispatcherAdapter_publishDeviceFound(ScanDispatcherHandle handle, uint64_t scanId, const char* host, int mmsPort,
        bool authRequired);

#endif /* SCAN_DISPATCHER_ADAPTER_H_ */
