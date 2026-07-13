#ifndef CONTROL_DISPATCHER_USECASES_H_
#define CONTROL_DISPATCHER_USECASES_H_

#include "features/control_dispatcher/domain/control_dispatcher_types.h"

/*
 * Dispatches one already-parsed ControlRequest to device_manager
 * (StartReporting/StopReporting) or scan_orchestration (StartScan/StopScan)
 * per request->type, and serializes the resulting success/error envelope via
 * control_dispatcher_json_writer - the one function the worker thread
 * (data/control_dispatcher_worker.c) calls per request. This is the only
 * place in this feature that maps DeviceManagerError/ScanOrchestrationError
 * to this feature's own stable JSON error codes (see CLAUDE.md's
 * control_dispatcher Architecture bullet for the full list).
 *
 * Returns an owned, heap-allocated JSON string (caller pushes it onto the
 * ring buffer, transferring ownership), or NULL only on cJSON allocation
 * failure (extremely unlikely - silently drops the response in that case,
 * no queue to retry onto).
 */
char*
ControlDispatcherUseCases_processRequest(const ControlRequest* request, DeviceManagerHandle deviceManager,
        ScanOrchestrationHandle scanOrchestration);

#endif /* CONTROL_DISPATCHER_USECASES_H_ */
