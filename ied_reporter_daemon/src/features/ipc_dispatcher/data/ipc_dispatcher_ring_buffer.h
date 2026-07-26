#ifndef IPC_DISPATCHER_RING_BUFFER_H_
#define IPC_DISPATCHER_RING_BUFFER_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Bounded, thread-safe broadcast ring of pre-serialized JSON strings (fixed
 * capacity, no dynamic growth - see CLAUDE.md's "no dangling connections"
 * ethos: bounded resources, not unbounded queues). Producer side: called
 * from mms_report_client's reconnect-supervisor thread AND goose_subscriber's
 * GooseReceiver reception thread (two independent producer threads). Reader
 * side: called only from ipc_dispatcher's own libwebsockets service-loop
 * thread, once per connected client per writable opportunity. Guarded
 * internally by a hal_thread.h Semaphore used as a binary mutex
 * (Semaphore_create(1)), same convention goose_subscriber already uses for
 * its own targetStateLock - hal_thread.h has no separate Mutex type.
 *
 * Opaque outside this file - nothing else needs field access, unlike every
 * sibling feature's struct s*Handle convention.
 */
typedef struct sIpcDispatcherRingBuffer* IpcDispatcherRingBuffer;

/* capacity must be > 0. Returns NULL on invalid argument or allocation failure. */
IpcDispatcherRingBuffer
IpcDispatcherRingBuffer_create(int capacity);

/* Frees the ring and every still-buffered JSON string it owns. NULL-safe. */
void
IpcDispatcherRingBuffer_destroy(IpcDispatcherRingBuffer buffer);

/*
 * Producer side. Takes ownership of `json` unconditionally - producer must
 * not touch it again after this call, including on failure (NULL buffer just
 * frees it). Never blocks, O(1): stores json at slot (head % capacity),
 * frees whatever owned string was previously in that slot (implicit
 * drop-oldest - a lagging reader detects the gap on its own next _readNext
 * call, see below; this call itself never knows or cares how far behind any
 * reader is), then advances head. Held under the internal mutex for the
 * whole operation (pointer swap only - no I/O, bounded and fast, safe to
 * call from a producer thread that must never block).
 */
void
IpcDispatcherRingBuffer_push(IpcDispatcherRingBuffer buffer, char* json);

/*
 * Reader side. `*cursor` is the caller's own next-unread sequence number -
 * initialize to IpcDispatcherRingBuffer_headSeq(buffer) at connection-
 * establish time (start-from-now, no backlog replay for a newly connected
 * client).
 *
 * - If *cursor == current head: nothing new, returns NULL, *outDroppedForThisRead
 *   (if non-NULL) set to 0.
 * - If head - *cursor > capacity (this reader's next-unread message was
 *   already overwritten): fast-forwards *cursor to (head - capacity),
 *   *outDroppedForThisRead (if non-NULL) is set to the number of messages
 *   skipped, and proceeds - so this call always returns the OLDEST message
 *   still actually available, never NULL purely due to lag.
 * - Otherwise: returns a freshly heap-allocated COPY of the message at
 *   *cursor (independent of the ring's own slot - safe to hold across
 *   multiple partial writes even if the producer later overwrites that
 *   slot) and advances *cursor by 1. Caller owns the returned string (free()
 *   when done).
 *
 * Safe to call concurrently with IpcDispatcherRingBuffer_push from a
 * different thread (both take the internal mutex); NOT safe to call this
 * function itself from more than one thread at a time for the SAME cursor
 * (each connection's cursor is only ever touched by the one service-loop
 * thread in this feature's usage, so this is a non-issue in practice).
 */
char*
IpcDispatcherRingBuffer_readNext(IpcDispatcherRingBuffer buffer, uint64_t* cursor, uint64_t* outDroppedForThisRead);

/* Current head sequence number - used to initialize a new connection's
 * cursor (start-from-now semantics). */
uint64_t
IpcDispatcherRingBuffer_headSeq(IpcDispatcherRingBuffer buffer);

/* Total number of messages ever pushed since creation - observational/
 * metrics only. */
uint64_t
IpcDispatcherRingBuffer_totalPushes(IpcDispatcherRingBuffer buffer);

#endif /* IPC_DISPATCHER_RING_BUFFER_H_ */
