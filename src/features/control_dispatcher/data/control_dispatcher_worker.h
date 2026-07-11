#ifndef CONTROL_DISPATCHER_WORKER_H_
#define CONTROL_DISPATCHER_WORKER_H_

#include "features/control_dispatcher/domain/control_dispatcher_types.h"
#include "features/control_dispatcher/data/control_dispatcher_request_queue.h"
#include "features/control_dispatcher/data/control_dispatcher_ring_buffer.h"
#include "features/control_dispatcher/data/control_dispatcher_ws_server.h"

/*
 * The dedicated thread that does the one genuinely slow thing in this
 * feature: calling device_manager's StartReporting/StopReporting (real
 * network I/O, seconds-scale) for one request at a time, off the
 * libwebsockets service thread entirely - see this feature's own
 * Architecture bullet in CLAUDE.md for why (the lws thread must never
 * block).
 *
 * Loop: Semaphore_wait(wake) -> pop the request queue -> if a request was
 * there, ControlDispatcherUseCases_processRequest (the slow part) -> push
 * the resulting JSON onto the ring buffer -> ControlDispatcherWsServer_wake
 * to have the lws thread write it out. Built entirely from primitives this
 * codebase already uses elsewhere - Semaphore_create(0) as a wake signal is
 * the exact idiom mms_report_client_connection.c's own supervisor thread
 * already uses.
 *
 * Opaque outside this file.
 */
typedef struct sControlDispatcherWorker* ControlDispatcherWorker;

/* Allocates + creates the wake semaphore only - no thread yet. queue/
 * ringBuffer/wsServer/deviceManager are all borrowed (owned/destroyed by
 * service/control_dispatcher_api.c). */
ControlDispatcherWorker
ControlDispatcherWorker_create(ControlDispatcherRequestQueue queue, ControlDispatcherRingBuffer ringBuffer,
        ControlDispatcherWsServer wsServer, DeviceManagerHandle deviceManager, ControlDispatcherError* outError);

/* Starts the dedicated worker thread. Non-blocking. Returns an error only on
 * Thread_create failure. */
ControlDispatcherError
ControlDispatcherWorker_start(ControlDispatcherWorker worker);

/*
 * Posts the wake semaphore - the ControlDispatcherRequestQueuedCallback
 * wired to ControlDispatcherWsServer_create (see service/
 * control_dispatcher_api.c). Safe to call from the lws thread; never blocks.
 */
void
ControlDispatcherWorker_notify(ControlDispatcherWorker worker);

/*
 * Signals the worker loop to exit, posts the wake semaphore once more to
 * guarantee a prompt exit even if idle (mirrors
 * ControlDispatcherWsServer_stop's own "wake even if idle" call, via a
 * semaphore post instead of lws_cancel_service), then blocks until it has
 * (bounded Thread_sleep poll - hal_thread.h has no Thread_join, same idiom
 * every other worker's stop function already uses). Anything still queued
 * at stop time is discarded, not drained - consistent with
 * ControlDispatcherWsServer_stop/IpcDispatcher_stop's own no-graceful-drain
 * behavior. MUST be called from the caller's own thread, never from within
 * a request's own processing (deadlock). Safe to call more than once /
 * before start (no-op).
 */
void
ControlDispatcherWorker_stop(ControlDispatcherWorker worker);

/* Implies ControlDispatcherWorker_stop() if still running. Frees the
 * handle. NULL-safe. */
void
ControlDispatcherWorker_destroy(ControlDispatcherWorker worker);

#endif /* CONTROL_DISPATCHER_WORKER_H_ */
