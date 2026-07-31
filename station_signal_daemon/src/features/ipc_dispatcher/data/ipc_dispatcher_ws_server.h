#ifndef IPC_DISPATCHER_WS_SERVER_H_
#define IPC_DISPATCHER_WS_SERVER_H_

#include <stdint.h>
#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ring_buffer.h"

/*
 * Owns the libwebsockets struct lws_context* and the ONE dedicated thread
 * that may ever touch it after creation (lws_write/lws_service/etc. are not
 * safe to call concurrently across threads for the same context - only
 * lws_cancel_service is documented safe to call from any thread, which is
 * exactly the cross-thread wakeup primitive this design relies on). Binds
 * 127.0.0.1 ONLY - not caller-configurable to any other address. No TLS.
 *
 * Opaque outside this file, same convention as ipc_dispatcher_ring_buffer.h.
 */
typedef struct sIpcDispatcherWsServer* IpcDispatcherWsServer;

/*
 * Performs the actual bind (lws_create_context with info.iface="127.0.0.1",
 * info.port=port) - this is the synchronous I/O step, deliberately NOT done
 * in IpcDispatcher_create (matches MmsReportClient_create/
 * GooseSubscription_create's own "no I/O at create" contract - I/O happens
 * at _start, which for this feature means IpcDispatcherWsServer_create+_start
 * called back-to-back from IpcDispatcher_start). ringBuffer is borrowed
 * (owned/destroyed by service/ipc_dispatcher_api.c). Returns NULL and sets
 * *outError to IPC_DISPATCHER_ERR_SOCKET_BIND_FAILED if lws_create_context
 * fails (e.g. port already in use).
 */
IpcDispatcherWsServer
IpcDispatcherWsServer_create(uint16_t port, int maxConnections,
        IpcDispatcherRingBuffer ringBuffer, IpcDispatcherError* outError);

/* Starts the dedicated service-loop thread (Thread_create/Thread_start).
 * Non-blocking. Returns an error only on Thread_create failure. */
IpcDispatcherError
IpcDispatcherWsServer_start(IpcDispatcherWsServer server);

/*
 * Producer-thread-safe wakeup: calls lws_cancel_service(context) - the ONE
 * libwebsockets call a producer thread (mms_report_client's supervisor /
 * goose_subscriber's GooseReceiver reception thread) may make directly.
 * Called by the data/ adapters immediately after a successful
 * IpcDispatcherRingBuffer_push. Never blocks.
 */
void
IpcDispatcherWsServer_wake(IpcDispatcherWsServer server);

/*
 * Sets (replacing any previous value) the retained "last known connection
 * status" JSON - copied internally (caller retains ownership of json).
 * Unlike report/GOOSE data (a pure stream of changes, deliberately no
 * backlog replay - see IpcDispatcherSession's own "start-from-now" comment
 * in the .c file), connection status is a STATE: a client that connects
 * after the status last changed still needs to know the current value, not
 * just future ones. Every newly-established connection is sent this value
 * (if any) as its first message, before draining the ring buffer as usual.
 * json == NULL clears the retained value (nothing will be replayed to new
 * connections until the next real status). Producer-thread-safe, same call
 * context as IpcDispatcherWsServer_wake - internally mutex-guarded.
 */
void
IpcDispatcherWsServer_setRetainedConnStatus(IpcDispatcherWsServer server, const char* json);

/*
 * Signals the service-loop thread to exit, wakes it via lws_cancel_service
 * to guarantee prompt exit even if idle, blocks until it has (bounded - same
 * convention as goose_subscriber's liveness-thread stop). MUST be called
 * from the caller's own thread, never from within a libwebsockets callback
 * (deadlock). Safe to call more than once / before start (no-op).
 */
void
IpcDispatcherWsServer_stop(IpcDispatcherWsServer server);

/* Implies IpcDispatcherWsServer_stop() if still running. Destroys the lws
 * context (closing the listener + every connected client) and frees the
 * handle. NULL-safe. */
void
IpcDispatcherWsServer_destroy(IpcDispatcherWsServer server);

#endif /* IPC_DISPATCHER_WS_SERVER_H_ */
