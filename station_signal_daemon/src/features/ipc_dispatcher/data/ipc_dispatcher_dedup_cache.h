#ifndef IPC_DISPATCHER_DEDUP_CACHE_H_
#define IPC_DISPATCHER_DEDUP_CACHE_H_

#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"

/*
 * INTERNAL to this feature - not part of the public boundary
 * (service/ipc_dispatcher_api.h is). Thread-safe wrapper around the pure
 * IpcDispatcherUseCases_shouldForwardWithinProtocol dedup logic
 * (domain/ipc_dispatcher_usecases.h) - one instance per protocol (see
 * sIpcDispatcherHandle's own mmsDedupCache/gooseDedupCache fields), each
 * guarding its own Semaphore so the MMS supervisor thread and the GOOSE
 * reception thread can call in concurrently without a shared lock forcing
 * one protocol's dedup check to serialize behind the other's - mirrors
 * goose_subscriber's own targetStateLock / mms_report_client's own
 * memberRefCacheLock idiom (one small Semaphore per guarded resource, not one
 * shared handle-wide lock). Kept opaque and out of
 * domain/ipc_dispatcher_types.h (deliberately zero-third-party) exactly like
 * ipc_dispatcher_ring_buffer/_ws_server's own structs - only this .c file
 * needs Semaphore/hal_thread.h.
 */

struct sIpcDispatcherDedupCache;
typedef struct sIpcDispatcherDedupCache* IpcDispatcherDedupCacheHandle;

/* Returns NULL on allocation failure. */
IpcDispatcherDedupCacheHandle
IpcDispatcherDedupCache_create(void);

/*
 * True (forward) if this (sourceId, reference, value, quality) should reach
 * the outbound message - see IpcDispatcherUseCases_shouldForwardWithinProtocol's
 * own doc comment for the full matching rule. Thread-safe: locks internally,
 * so the caller needs no lock of its own around this call. A NULL handle is
 * treated as "always forward" (defense-in-depth, matches the underlying pure
 * function's own NULL-cache behavior).
 */
bool
IpcDispatcherDedupCache_shouldForward(IpcDispatcherDedupCacheHandle handle, const char* sourceId,
        const char* reference, const IpcScalarValue* value, bool hasQuality, IpcQuality quality);

/* Frees every owned entry plus the lock itself. NULL-safe. */
void
IpcDispatcherDedupCache_destroy(IpcDispatcherDedupCacheHandle handle);

#endif /* IPC_DISPATCHER_DEDUP_CACHE_H_ */
