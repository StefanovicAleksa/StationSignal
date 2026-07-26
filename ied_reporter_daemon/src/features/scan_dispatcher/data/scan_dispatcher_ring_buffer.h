#ifndef SCAN_DISPATCHER_RING_BUFFER_H_
#define SCAN_DISPATCHER_RING_BUFFER_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Bounded, thread-safe broadcast ring of pre-serialized JSON strings (fixed
 * capacity, no dynamic growth - see CLAUDE.md's "no dangling connections"
 * ethos: bounded resources, not unbounded queues). Near-verbatim port of
 * ipc_dispatcher_ring_buffer.{h,c} - duplicated deliberately rather than
 * shared, see scan_dispatcher_types.h's own top comment.
 *
 * Producer side: called from every active scan's own worker thread
 * (scan_orchestration's per-scan sweep loop) - potentially multiple
 * concurrent producer threads, same as ipc_dispatcher's two independent
 * producers. Reader side: called only from this feature's own libwebsockets
 * service-loop thread, once per connected client per writable opportunity.
 * Guarded internally by a hal_thread.h Semaphore used as a binary mutex
 * (Semaphore_create(1)), same convention as ipc_dispatcher_ring_buffer.h -
 * hal_thread.h has no separate Mutex type.
 *
 * Opaque outside this file - nothing else needs field access.
 */
typedef struct sScanDispatcherRingBuffer* ScanDispatcherRingBuffer;

/* capacity must be > 0. Returns NULL on invalid argument or allocation failure. */
ScanDispatcherRingBuffer
ScanDispatcherRingBuffer_create(int capacity);

/* Frees the ring and every still-buffered JSON string it owns. NULL-safe. */
void
ScanDispatcherRingBuffer_destroy(ScanDispatcherRingBuffer buffer);

/*
 * Producer side. Takes ownership of `json` unconditionally - producer must
 * not touch it again after this call, including on failure (NULL buffer just
 * frees it). Never blocks, O(1): stores json at slot (head % capacity),
 * frees whatever owned string was previously in that slot (implicit
 * drop-oldest), then advances head.
 */
void
ScanDispatcherRingBuffer_push(ScanDispatcherRingBuffer buffer, char* json);

/*
 * Reader side. `*cursor` is the caller's own next-unread sequence number -
 * initialize to ScanDispatcherRingBuffer_headSeq(buffer) at connection-
 * establish time (start-from-now, no backlog replay for a newly connected
 * client).
 *
 * - If *cursor == current head: nothing new, returns NULL, *outDroppedForThisRead
 *   (if non-NULL) set to 0.
 * - If head - *cursor > capacity (this reader's next-unread message was
 *   already overwritten): fast-forwards *cursor to (head - capacity),
 *   *outDroppedForThisRead (if non-NULL) is set to the number of messages
 *   skipped, and proceeds.
 * - Otherwise: returns a freshly heap-allocated COPY of the message at
 *   *cursor and advances *cursor by 1. Caller owns the returned string
 *   (free() when done).
 */
char*
ScanDispatcherRingBuffer_readNext(ScanDispatcherRingBuffer buffer, uint64_t* cursor, uint64_t* outDroppedForThisRead);

/* Current head sequence number - used to initialize a new connection's
 * cursor (start-from-now semantics). */
uint64_t
ScanDispatcherRingBuffer_headSeq(ScanDispatcherRingBuffer buffer);

#endif /* SCAN_DISPATCHER_RING_BUFFER_H_ */
