#ifndef CONTROL_DISPATCHER_WS_SERVER_H_
#define CONTROL_DISPATCHER_WS_SERVER_H_

#include <stdint.h>
#include "features/control_dispatcher/domain/control_dispatcher_types.h"
#include "features/control_dispatcher/data/control_dispatcher_ring_buffer.h"
#include "features/control_dispatcher/data/control_dispatcher_request_queue.h"

/*
 * Owns the libwebsockets struct lws_context* and the ONE dedicated thread
 * that may ever touch it after creation - adapted from
 * ipc_dispatcher_ws_server.{h,c}/scan_dispatcher_ws_server.{h,c} (see
 * domain/control_dispatcher_types.h's own top comment for why duplicated
 * rather than shared), with ONE structural addition those push-only
 * transports don't have: LWS_CALLBACK_RECEIVE handling, since this feature
 * is bidirectional. Binds 127.0.0.1 ONLY. No TLS.
 *
 * Inbound handling (per connection, accumulated in a bounded per-session
 * buffer until the final fragment of one text frame arrives): parses via
 * ControlDispatcherJsonParser_parse. A parse/validation failure OR a full
 * request queue is handled ENTIRELY on this thread - builds the error JSON
 * directly (via control_dispatcher_json_writer.h) and pushes it straight
 * onto the ring buffer, never touching onRequestQueued/the worker thread. A
 * successfully parsed+queued request instead triggers onRequestQueued
 * (deliberately a generic function-pointer callback, not a direct
 * dependency on the worker's own type, so this file never needs to include
 * control_dispatcher_worker.h - the worker is the one thing depending on
 * THIS file, for _wake(), not the other way around).
 *
 * Opaque outside this file, same convention as
 * control_dispatcher_ring_buffer.h.
 */
typedef struct sControlDispatcherWsServer* ControlDispatcherWsServer;

/* Called synchronously on the lws thread, immediately after a request has
 * been successfully queued - implementation MUST NOT block (typically just
 * posts the worker's own wake semaphore). */
typedef void (*ControlDispatcherRequestQueuedCallback)(void* userParam);

/*
 * Performs the actual bind (lws_create_context with info.iface="127.0.0.1",
 * info.port=port) - deliberately NOT done at ControlDispatcher_create time
 * (matches every other feature's "no I/O at create" contract). ringBuffer/
 * requestQueue are borrowed (owned/destroyed by service/
 * control_dispatcher_api.c). Returns NULL and sets *outError to
 * CONTROL_DISPATCHER_ERR_SOCKET_BIND_FAILED if lws_create_context fails
 * (e.g. port already in use).
 */
ControlDispatcherWsServer
ControlDispatcherWsServer_create(uint16_t port, int maxConnections, ControlDispatcherRingBuffer ringBuffer,
        ControlDispatcherRequestQueue requestQueue, ControlDispatcherRequestQueuedCallback onRequestQueued,
        void* onRequestQueuedParam, ControlDispatcherError* outError);

/* Starts the dedicated service-loop thread (Thread_create/Thread_start).
 * Non-blocking. Returns an error only on Thread_create failure. */
ControlDispatcherError
ControlDispatcherWsServer_start(ControlDispatcherWsServer server);

/*
 * Sets/replaces the request-queued callback + param after creation - needed
 * because the callback's param is typically the worker handle, which itself
 * needs a reference to THIS already-created ws server (to wake it after
 * processing) - see service/control_dispatcher_api.c's own creation order
 * for the resolution of this mutual dependency. Safe to call before
 * _start(); not meant to be changed while running.
 */
void
ControlDispatcherWsServer_setRequestQueuedCallback(ControlDispatcherWsServer server,
        ControlDispatcherRequestQueuedCallback callback, void* param);

/*
 * Producer-thread-safe wakeup: calls lws_cancel_service(context) - the ONE
 * libwebsockets call a producer thread (the worker thread, after pushing a
 * response) may make directly. Never blocks.
 */
void
ControlDispatcherWsServer_wake(ControlDispatcherWsServer server);

/*
 * Signals the service-loop thread to exit, wakes it via lws_cancel_service
 * to guarantee prompt exit even if idle, blocks until it has. MUST be called
 * from the caller's own thread, never from within a libwebsockets callback
 * (deadlock). Safe to call more than once / before start (no-op).
 */
void
ControlDispatcherWsServer_stop(ControlDispatcherWsServer server);

/* Implies ControlDispatcherWsServer_stop() if still running. Destroys the
 * lws context (closing the listener + every connected client) and frees the
 * handle. NULL-safe. */
void
ControlDispatcherWsServer_destroy(ControlDispatcherWsServer server);

#endif /* CONTROL_DISPATCHER_WS_SERVER_H_ */
