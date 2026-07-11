#ifndef CONTROL_DISPATCHER_RING_BUFFER_H_
#define CONTROL_DISPATCHER_RING_BUFFER_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Bounded, thread-safe broadcast ring of pre-serialized JSON response
 * strings - near-verbatim duplicate of ipc_dispatcher_ring_buffer.{h,c}/
 * scan_dispatcher_ring_buffer.{h,c} (see this feature's own
 * domain/control_dispatcher_types.h top comment for why duplicated rather
 * than shared). Producer side here is this feature's OWN worker thread
 * (control_dispatcher_worker.c), not an external MMS/GOOSE producer - it
 * pushes one response per finished request. Reader side: only
 * control_dispatcher's own libwebsockets service-loop thread, once per
 * connected client per writable opportunity.
 *
 * Opaque outside this file.
 */
typedef struct sControlDispatcherRingBuffer* ControlDispatcherRingBuffer;

/* capacity must be > 0. Returns NULL on invalid argument or allocation failure. */
ControlDispatcherRingBuffer
ControlDispatcherRingBuffer_create(int capacity);

/* Frees the ring and every still-buffered JSON string it owns. NULL-safe. */
void
ControlDispatcherRingBuffer_destroy(ControlDispatcherRingBuffer buffer);

/* Producer side. Takes ownership of `json` unconditionally - never blocks,
 * O(1). See ipc_dispatcher_ring_buffer.h's own doc comment for the full
 * drop-oldest/lag semantics - identical here. */
void
ControlDispatcherRingBuffer_push(ControlDispatcherRingBuffer buffer, char* json);

/* Reader side - identical cursor/lag-detection contract to
 * ipc_dispatcher_ring_buffer.h's own IpcDispatcherRingBuffer_readNext.
 * Caller owns the returned string (free() when done). */
char*
ControlDispatcherRingBuffer_readNext(ControlDispatcherRingBuffer buffer, uint64_t* cursor,
        uint64_t* outDroppedForThisRead);

/* Current head sequence number - used to initialize a new connection's
 * cursor (start-from-now semantics). */
uint64_t
ControlDispatcherRingBuffer_headSeq(ControlDispatcherRingBuffer buffer);

#endif /* CONTROL_DISPATCHER_RING_BUFFER_H_ */
