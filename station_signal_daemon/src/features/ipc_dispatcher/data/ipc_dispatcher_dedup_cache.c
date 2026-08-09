#include <stdlib.h>
#include "features/ipc_dispatcher/data/ipc_dispatcher_dedup_cache.h"
#include "features/ipc_dispatcher/domain/ipc_dispatcher_usecases.h"
#include "hal_thread.h"

struct sIpcDispatcherDedupCache {
    IpcDispatcherDedupCache cache; /* zero-initialized via calloc below */
    Semaphore lock;
};

IpcDispatcherDedupCacheHandle
IpcDispatcherDedupCache_create(void) {
    struct sIpcDispatcherDedupCache* handle = calloc(1, sizeof(struct sIpcDispatcherDedupCache));
    if (!handle) return NULL;

    handle->lock = Semaphore_create(1);
    return handle;
}

bool
IpcDispatcherDedupCache_shouldForward(IpcDispatcherDedupCacheHandle handle, const char* sourceId,
        const char* reference, const IpcScalarValue* value, bool hasQuality, IpcQuality quality) {
    if (!handle) return true;

    Semaphore_wait(handle->lock);
    bool result = IpcDispatcherUseCases_shouldForwardWithinProtocol(&handle->cache, sourceId, reference, value,
            hasQuality, quality);
    Semaphore_post(handle->lock);

    return result;
}

void
IpcDispatcherDedupCache_destroy(IpcDispatcherDedupCacheHandle handle) {
    if (!handle) return;

    IpcDispatcherUseCases_destroyDedupCache(&handle->cache);
    if (handle->lock) Semaphore_destroy(handle->lock);
    free(handle);
}
