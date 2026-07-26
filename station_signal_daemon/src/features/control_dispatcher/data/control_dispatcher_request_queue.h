#ifndef CONTROL_DISPATCHER_REQUEST_QUEUE_H_
#define CONTROL_DISPATCHER_REQUEST_QUEUE_H_

#include "stdbool_compat.h"
#include "features/control_dispatcher/domain/control_dispatcher_types.h"

/*
 * Bounded FIFO of parsed ControlRequest* - lws thread pushes (one per
 * successfully-parsed inbound command), worker thread pops (one per
 * iteration of its own request-processing loop). Own Semaphore(1)-as-mutex,
 * deliberately kept separate from control_dispatcher_ring_buffer's own lock
 * (different producer/consumer thread pairs: this queue is
 * lws-thread-pushes/worker-thread-pops, the ring buffer is
 * worker-thread-pushes/lws-thread-reads - reusing one lock for both would be
 * safe but couples two independent producer/consumer relationships for no
 * benefit).
 *
 * Opaque outside this file.
 */
typedef struct sControlDispatcherRequestQueue* ControlDispatcherRequestQueue;

/* capacity must be > 0. Returns NULL on invalid argument or allocation failure. */
ControlDispatcherRequestQueue
ControlDispatcherRequestQueue_create(int capacity);

/* Frees the queue and every still-queued ControlRequest it owns (via
 * ControlDispatcherRequest_destroy). NULL-safe. */
void
ControlDispatcherRequestQueue_destroy(ControlDispatcherRequestQueue queue);

/* Producer side (lws thread). Takes ownership of `request` on success.
 * Returns false (caller still owns `request`) if the queue is at capacity -
 * the lws thread treats this as SERVER_BUSY and must free `request` itself
 * (via ControlDispatcherRequest_destroy). Never blocks. */
bool
ControlDispatcherRequestQueue_push(ControlDispatcherRequestQueue queue, ControlRequest* request);

/* Consumer side (worker thread). Returns NULL if empty (a spurious wake is
 * always tolerated by the worker's own loop). Caller owns the returned
 * request (must eventually call ControlDispatcherRequest_destroy). Never
 * blocks. */
ControlRequest*
ControlDispatcherRequestQueue_pop(ControlDispatcherRequestQueue queue);

/* Frees one ControlRequest and every owned string field it holds. NULL-safe. */
void
ControlDispatcherRequest_destroy(ControlRequest* request);

#endif /* CONTROL_DISPATCHER_REQUEST_QUEUE_H_ */
