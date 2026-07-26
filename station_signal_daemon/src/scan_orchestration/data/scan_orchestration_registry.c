#include <stdlib.h>
#include "hal_thread.h"
#include "scan_orchestration/data/scan_orchestration_registry.h"
#include "features/scan_dispatcher/service/scan_dispatcher_api.h"

typedef struct {
    uint64_t scanId;
    ScanOrchestrationWorker worker;
} ScanOrchestrationRegistryEntry;

struct sScanOrchestrationRegistry {
    Semaphore lock;
    ScanOrchestrationRegistryEntry* entries; /* owned array */
    int count;
    int capacity;
    uint64_t nextScanId; /* starts at 1 */
};

ScanOrchestrationRegistry
ScanOrchestrationRegistry_create(void) {
    struct sScanOrchestrationRegistry* registry = calloc(1, sizeof(struct sScanOrchestrationRegistry));
    if (!registry) return NULL;

    registry->lock = Semaphore_create(1);
    if (!registry->lock) {
        free(registry);
        return NULL;
    }

    registry->nextScanId = 1;
    return registry;
}

void
ScanOrchestrationRegistry_destroy(ScanOrchestrationRegistry registry) {
    if (!registry) return;

    if (registry->lock) Semaphore_destroy(registry->lock);
    free(registry->entries);
    free(registry);
}

uint64_t
ScanOrchestrationRegistry_nextScanId(ScanOrchestrationRegistry registry) {
    if (!registry) return 0;

    Semaphore_wait(registry->lock);
    uint64_t id = registry->nextScanId++;
    Semaphore_post(registry->lock);

    return id;
}

static bool
appendEntry(struct sScanOrchestrationRegistry* registry, uint64_t scanId, ScanOrchestrationWorker worker) {
    if (registry->count >= registry->capacity) {
        int newCapacity = (registry->capacity > 0) ? (registry->capacity * 2) : 8;
        ScanOrchestrationRegistryEntry* grown =
                realloc(registry->entries, (size_t) newCapacity * sizeof(ScanOrchestrationRegistryEntry));
        if (!grown) return false;
        registry->entries = grown;
        registry->capacity = newCapacity;
    }

    registry->entries[registry->count].scanId = scanId;
    registry->entries[registry->count].worker = worker;
    registry->count++;
    return true;
}

ScanOrchestrationError
ScanOrchestrationRegistry_addAndMaybeStartDispatcher(ScanOrchestrationRegistry registry,
        ScanDispatcherHandle scanDispatcher, ScanOrchestrationWorker worker) {
    if (!registry || !worker) return SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT;

    Semaphore_wait(registry->lock);

    ScanOrchestrationError result = SCAN_ORCHESTRATION_OK;

    if (registry->count == 0) {
        ScanDispatcherError dispatcherErr = ScanDispatcher_start(scanDispatcher);
        if (dispatcherErr != SCAN_DISPATCHER_OK) {
            Semaphore_post(registry->lock);
            return SCAN_ORCHESTRATION_ERR_DISPATCHER_START_FAILED;
        }
    }

    if (!appendEntry(registry, ScanOrchestrationWorker_scanId(worker), worker)) {
        /* Registration itself failed (OOM) - if we just started the dispatcher for this
         * would-be-first scan, undo that so a subsequent successful startScan can rebind cleanly. */
        if (registry->count == 0) ScanDispatcher_stop(scanDispatcher);
        result = SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY;
    }

    Semaphore_post(registry->lock);
    return result;
}

bool
ScanOrchestrationRegistry_remove(ScanOrchestrationRegistry registry, uint64_t scanId,
        ScanOrchestrationWorker* outWorker, bool* outNowEmpty) {
    if (outNowEmpty) *outNowEmpty = false;
    if (!registry) return false;

    Semaphore_wait(registry->lock);

    int foundIndex = -1;
    for (int i = 0; i < registry->count; i++) {
        if (registry->entries[i].scanId == scanId) {
            foundIndex = i;
            break;
        }
    }

    if (foundIndex < 0) {
        Semaphore_post(registry->lock);
        return false;
    }

    if (outWorker) *outWorker = registry->entries[foundIndex].worker;

    /* Swap-remove - order among active scans is never meaningful. */
    registry->entries[foundIndex] = registry->entries[registry->count - 1];
    registry->count--;

    if (outNowEmpty) *outNowEmpty = (registry->count == 0);

    Semaphore_post(registry->lock);
    return true;
}

ScanOrchestrationWorker
ScanOrchestrationRegistry_find(ScanOrchestrationRegistry registry, uint64_t scanId) {
    if (!registry) return NULL;

    Semaphore_wait(registry->lock);
    ScanOrchestrationWorker found = NULL;
    for (int i = 0; i < registry->count; i++) {
        if (registry->entries[i].scanId == scanId) {
            found = registry->entries[i].worker;
            break;
        }
    }
    Semaphore_post(registry->lock);

    return found;
}

int
ScanOrchestrationRegistry_activeCount(ScanOrchestrationRegistry registry) {
    if (!registry) return 0;

    Semaphore_wait(registry->lock);
    int count = registry->count;
    Semaphore_post(registry->lock);

    return count;
}

uint64_t
ScanOrchestrationRegistry_anyActiveScanId(ScanOrchestrationRegistry registry) {
    if (!registry) return 0;

    Semaphore_wait(registry->lock);
    uint64_t scanId = (registry->count > 0) ? registry->entries[0].scanId : 0;
    Semaphore_post(registry->lock);

    return scanId;
}
