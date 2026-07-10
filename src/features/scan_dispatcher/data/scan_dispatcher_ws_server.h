#ifndef SCAN_DISPATCHER_WS_SERVER_H_
#define SCAN_DISPATCHER_WS_SERVER_H_

#include <stdint.h>
#include "features/scan_dispatcher/domain/scan_dispatcher_types.h"
#include "features/scan_dispatcher/data/scan_dispatcher_ring_buffer.h"

/*
 * Owns the libwebsockets struct lws_context* and the ONE dedicated thread
 * that may ever touch it after creation - near-verbatim port of
 * ipc_dispatcher_ws_server.{h,c}, see scan_dispatcher_types.h's own top
 * comment for why this is duplicated rather than shared. Binds 127.0.0.1
 * ONLY. No TLS.
 *
 * Opaque outside this file, same convention as scan_dispatcher_ring_buffer.h.
 */
typedef struct sScanDispatcherWsServer* ScanDispatcherWsServer;

/*
 * Performs the actual bind (lws_create_context with info.iface="127.0.0.1",
 * info.port=port) - deliberately NOT done in ScanDispatcher_create (matches
 * every other feature's "no I/O at create" contract). ringBuffer is borrowed
 * (owned/destroyed by service/scan_dispatcher_api.c). Returns NULL and sets
 * *outError to SCAN_DISPATCHER_ERR_SOCKET_BIND_FAILED if lws_create_context
 * fails (e.g. port already in use).
 */
ScanDispatcherWsServer
ScanDispatcherWsServer_create(uint16_t port, int maxConnections,
        ScanDispatcherRingBuffer ringBuffer, ScanDispatcherError* outError);

/* Starts the dedicated service-loop thread (Thread_create/Thread_start).
 * Non-blocking. Returns an error only on Thread_create failure. */
ScanDispatcherError
ScanDispatcherWsServer_start(ScanDispatcherWsServer server);

/*
 * Producer-thread-safe wakeup: calls lws_cancel_service(context) - the ONE
 * libwebsockets call a producer thread (a scan's own worker thread) may make
 * directly. Called immediately after a successful
 * ScanDispatcherRingBuffer_push. Never blocks.
 */
void
ScanDispatcherWsServer_wake(ScanDispatcherWsServer server);

/*
 * Signals the service-loop thread to exit, wakes it via lws_cancel_service
 * to guarantee prompt exit even if idle, blocks until it has. MUST be called
 * from the caller's own thread, never from within a libwebsockets callback
 * (deadlock). Safe to call more than once / before start (no-op).
 */
void
ScanDispatcherWsServer_stop(ScanDispatcherWsServer server);

/* Implies ScanDispatcherWsServer_stop() if still running. Destroys the lws
 * context (closing the listener + every connected client) and frees the
 * handle. NULL-safe. */
void
ScanDispatcherWsServer_destroy(ScanDispatcherWsServer server);

#endif /* SCAN_DISPATCHER_WS_SERVER_H_ */
