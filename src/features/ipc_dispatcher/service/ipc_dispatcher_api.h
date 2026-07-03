#ifndef IPC_DISPATCHER_API_H_
#define IPC_DISPATCHER_API_H_

#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"
#include "features/mms_report_client/service/mms_report_client_api.h"
#include "features/goose_subscriber/service/goose_subscriber_api.h"

/*
 * Public boundary of ipc_dispatcher. Other code (orchestration, main.c)
 * should only ever include this header - never reach into domain/data/utils
 * directly.
 *
 * Relays normalized MMS-report/GOOSE records over a loopback-only websocket
 * for a separate API layer/frontend to consume. Push-only (no client-sent
 * message handling). Single-IED scope, matching the whole daemon's scope.
 */

/* Fills config with the recommended defaults (port=8765, ringBufferCapacity=256,
 * maxConnections=16). Caller may then override individual fields before
 * passing to IpcDispatcher_create. */
void
IpcDispatcherConfig_defaults(IpcDispatcherConfig* config);

/*
 * Allocates only - no socket/thread yet (creates the ring buffer, which is
 * pure allocation, no I/O), matching MmsReportClient_create/
 * GooseSubscription_create's own "no I/O at create" contract. config: NULL
 * means IpcDispatcherConfig_defaults(). Returns NULL and sets *outError on
 * argument/allocation failure only.
 */
IpcDispatcherHandle
IpcDispatcher_create(const IpcDispatcherConfig* config, IpcDispatcherError* outError);

/*
 * Binds 127.0.0.1:config.port and starts the dedicated libwebsockets
 * service-loop thread. Non-blocking once the synchronous bind succeeds.
 * Returns IPC_DISPATCHER_ERR_ALREADY_RUNNING if called twice without an
 * intervening _stop(). Returns IPC_DISPATCHER_ERR_SOCKET_BIND_FAILED if the
 * port is unavailable.
 */
IpcDispatcherError
IpcDispatcher_start(IpcDispatcherHandle handle);

/*
 * Stops the service-loop thread (bounded, prompt - woken via
 * lws_cancel_service) and closes the listening socket + every connected
 * client. Blocking. MUST be called from the caller's own thread, never from
 * within either callback below (deadlock - same rule as every other
 * feature's _stop()). Safe to call more than once / on a never-started
 * handle (no-op).
 */
void
IpcDispatcher_stop(IpcDispatcherHandle handle);

/* Implies IpcDispatcher_stop() if still running. Frees the handle
 * (including the ring buffer). */
void
IpcDispatcher_destroy(IpcDispatcherHandle handle);

/*
 * Callback-adapter matching MmsReportClientCallback's signature exactly.
 * Register directly:
 *   Orchestration_setReportCallback(orchestrationHandle, IpcDispatcher_onMmsReport, dispatcherHandle);
 * userParam MUST be the IpcDispatcherHandle (this function casts it back).
 * Extracts, pairs quality, serializes to JSON, and enqueues onto the
 * internal ring buffer - fast, non-blocking, never touches a struct lws*
 * directly - safe to call from mms_report_client's reconnect-supervisor
 * thread. ALWAYS calls MmsReportClient_destroyReportRecord(record) before
 * returning (takes ownership per that feature's callback contract) - the
 * caller must NOT also destroy it.
 */
void
IpcDispatcher_onMmsReport(void* userParam, const MmsReportRecord* record);

/*
 * Same contract as IpcDispatcher_onMmsReport, for GOOSE. Register via
 *   Orchestration_setGooseRecordCallback(orchestrationHandle, IpcDispatcher_onGooseRecord, dispatcherHandle);
 * Always calls GooseSubscription_destroyRecord(record) before returning.
 * Safe to call from goose_subscriber's GooseReceiver reception thread.
 */
void
IpcDispatcher_onGooseRecord(void* userParam, const GooseSubscriberRecord* record);

#endif /* IPC_DISPATCHER_API_H_ */
