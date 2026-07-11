#ifndef CONTROL_DISPATCHER_API_H_
#define CONTROL_DISPATCHER_API_H_

#include "features/control_dispatcher/domain/control_dispatcher_types.h"

/*
 * Public boundary of control_dispatcher. Other code (main.c) should only
 * ever include this header - never reach into domain/data directly.
 *
 * Unlike ipc_dispatcher/scan_dispatcher (push-only transports), this
 * feature RECEIVES JSON commands (START_REPORTING/STOP_REPORTING) over its
 * one well-known websocket port and pushes back JSON acks/errors, relaying
 * them to device_manager - see CLAUDE.md's control_dispatcher Architecture
 * bullet for the full envelope shape, threading design, and error-code
 * table. deviceManager is BORROWED at _create time - caller (main.c) owns
 * its lifetime and must keep it alive for at least this handle's own
 * lifetime (create..destroy).
 */

/* Fills config with recommended defaults: port=8767 (next after
 * ipc_dispatcher's 8765 and scan_dispatcher's 8766), ringBufferCapacity=256,
 * maxConnections=16, requestQueueCapacity=64. Caller may override individual
 * fields before passing to ControlDispatcher_create. */
void
ControlDispatcherConfig_defaults(ControlDispatcherConfig* config);

/*
 * Allocates only (ring buffer + request queue, both pure allocation) - no
 * bind/thread yet, matching every sibling feature's "no I/O at create"
 * contract. config: NULL means ControlDispatcherConfig_defaults(). Returns
 * NULL and sets *outError on argument/allocation failure only.
 */
ControlDispatcherHandle
ControlDispatcher_create(const ControlDispatcherConfig* config, DeviceManagerHandle deviceManager,
        ControlDispatcherError* outError);

/*
 * Binds 127.0.0.1:config.port, starts the libwebsockets service thread AND
 * the dedicated request-worker thread. Non-blocking once the synchronous
 * bind succeeds. Returns CONTROL_DISPATCHER_ERR_ALREADY_RUNNING if called
 * twice without an intervening _stop(). Returns
 * CONTROL_DISPATCHER_ERR_SOCKET_BIND_FAILED if the port is unavailable (the
 * worker thread is never started in that case).
 */
ControlDispatcherError
ControlDispatcher_start(ControlDispatcherHandle handle);

/*
 * Stops the worker thread first (discards anything still queued - no
 * graceful drain, matches IpcDispatcher_stop's own behavior), then the
 * ws-server thread (fully tears down the lws context, same as
 * ipc_dispatcher/scan_dispatcher's own _stop - a subsequent
 * ControlDispatcher_start on the same handle can cleanly rebind the same
 * port). Blocking. MUST be called from the caller's own thread, never from
 * within a callback (deadlock). Safe to call more than once / on a
 * never-started handle (no-op).
 */
void
ControlDispatcher_stop(ControlDispatcherHandle handle);

/* Implies ControlDispatcher_stop() if still running. Frees the handle
 * (including the ring buffer and request queue). Does NOT touch the
 * borrowed DeviceManagerHandle. NULL-safe. */
void
ControlDispatcher_destroy(ControlDispatcherHandle handle);

#endif /* CONTROL_DISPATCHER_API_H_ */
