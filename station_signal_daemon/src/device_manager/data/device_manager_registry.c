#include <stdlib.h>
#include <string.h>
#include "hal_thread.h"
#include "device_manager/data/device_manager_registry.h"
#include "device_manager/data/device_manager_port_allocator.h"
#include "orchestration/utils/orchestration_utils.h"

typedef struct {
    uint64_t deviceId;
    uint16_t wsPort;
    bool running;                    /* false while still in the slow Orchestration_run* phase */
    char* host;                      /* owned copy - dedupe check + diagnostics only */
    int mmsPort;
    char* ownedAcseAuthPassword;     /* owned copy or NULL - see this header's own top comment */
    OrchestrationHandle orchestrationHandle; /* NULL until the finalize phase attaches it */
} DeviceManagerRegistryEntry;

struct sDeviceManagerRegistry {
    Semaphore lock;                     /* hal_thread.h Semaphore(1) as binary mutex */
    DeviceManagerRegistryEntry* entries; /* owned array, swap-remove on stop/rollback */
    int count;
    int capacity;
    uint64_t nextDeviceId;              /* starts at 1, same convention as scan_orchestration's scanId */
    DeviceManagerPortAllocator ports;   /* owned */
};

DeviceManagerRegistry
DeviceManagerRegistry_create(uint16_t wsPortRangeStart, uint16_t wsPortRangeEnd) {
    struct sDeviceManagerRegistry* registry = calloc(1, sizeof(struct sDeviceManagerRegistry));
    if (!registry) return NULL;

    registry->lock = Semaphore_create(1);
    if (!registry->lock) {
        free(registry);
        return NULL;
    }

    registry->ports = DeviceManagerPortAllocator_create(wsPortRangeStart, wsPortRangeEnd);
    if (!registry->ports) {
        Semaphore_destroy(registry->lock);
        free(registry);
        return NULL;
    }

    registry->nextDeviceId = 1;
    return registry;
}

void
DeviceManagerRegistry_destroy(DeviceManagerRegistry registry) {
    if (!registry) return;

    for (int i = 0; i < registry->count; i++) {
        free(registry->entries[i].host);
        free(registry->entries[i].ownedAcseAuthPassword);
    }
    free(registry->entries);
    if (registry->ports) DeviceManagerPortAllocator_destroy(registry->ports);
    if (registry->lock) Semaphore_destroy(registry->lock);
    free(registry);
}

static bool
appendEntry(struct sDeviceManagerRegistry* registry, const DeviceManagerRegistryEntry* entry) {
    if (registry->count >= registry->capacity) {
        int newCapacity = (registry->capacity > 0) ? (registry->capacity * 2) : 8;
        DeviceManagerRegistryEntry* grown =
                realloc(registry->entries, (size_t) newCapacity * sizeof(DeviceManagerRegistryEntry));
        if (!grown) return false;
        registry->entries = grown;
        registry->capacity = newCapacity;
    }

    registry->entries[registry->count] = *entry;
    registry->count++;
    return true;
}

static bool
hostAlreadyActive(struct sDeviceManagerRegistry* registry, const char* host, int mmsPort) {
    for (int i = 0; i < registry->count; i++) {
        if (registry->entries[i].mmsPort == mmsPort && registry->entries[i].host
                && strcmp(registry->entries[i].host, host) == 0) {
            return true;
        }
    }
    return false;
}

static int
findIndexByDeviceId(struct sDeviceManagerRegistry* registry, uint64_t deviceId) {
    for (int i = 0; i < registry->count; i++) {
        if (registry->entries[i].deviceId == deviceId) return i;
    }
    return -1;
}

DeviceManagerError
DeviceManagerRegistry_reserve(DeviceManagerRegistry registry, const char* host, int mmsPort,
        const char* acseAuthPassword, uint64_t* outDeviceId, uint16_t* outWsPort,
        const char** outOwnedAcseAuthPassword) {
    if (!registry || !host || !outDeviceId || !outWsPort) return DEVICE_MANAGER_ERR_INVALID_ARGUMENT;

    Semaphore_wait(registry->lock);

    if (hostAlreadyActive(registry, host, mmsPort)) {
        Semaphore_post(registry->lock);
        return DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING;
    }

    uint16_t port;
    if (!DeviceManagerPortAllocator_alloc(registry->ports, &port)) {
        Semaphore_post(registry->lock);
        return DEVICE_MANAGER_ERR_PORT_EXHAUSTED;
    }

    char* ownedHost = OrchestrationUtils_safeStringDup(host);
    char* ownedPassword = OrchestrationUtils_safeStringDup(acseAuthPassword);
    if (!ownedHost || (acseAuthPassword && !ownedPassword)) {
        DeviceManagerPortAllocator_free(registry->ports, port);
        free(ownedHost);
        free(ownedPassword);
        Semaphore_post(registry->lock);
        return DEVICE_MANAGER_ERR_OUT_OF_MEMORY;
    }

    DeviceManagerRegistryEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.deviceId = registry->nextDeviceId;
    entry.wsPort = port;
    entry.running = false;
    entry.host = ownedHost;
    entry.mmsPort = mmsPort;
    entry.ownedAcseAuthPassword = ownedPassword;
    entry.orchestrationHandle = NULL;

    if (!appendEntry(registry, &entry)) {
        DeviceManagerPortAllocator_free(registry->ports, port);
        free(ownedHost);
        free(ownedPassword);
        Semaphore_post(registry->lock);
        return DEVICE_MANAGER_ERR_OUT_OF_MEMORY;
    }

    uint64_t deviceId = registry->nextDeviceId;
    registry->nextDeviceId++;

    Semaphore_post(registry->lock);

    *outDeviceId = deviceId;
    *outWsPort = port;
    if (outOwnedAcseAuthPassword) *outOwnedAcseAuthPassword = ownedPassword;
    return DEVICE_MANAGER_OK;
}

void
DeviceManagerRegistry_finalize(DeviceManagerRegistry registry, uint64_t deviceId, OrchestrationHandle handle) {
    if (!registry) return;

    Semaphore_wait(registry->lock);
    int index = findIndexByDeviceId(registry, deviceId);
    if (index >= 0) {
        registry->entries[index].orchestrationHandle = handle;
        registry->entries[index].running = true;
    }
    Semaphore_post(registry->lock);
}

void
DeviceManagerRegistry_rollback(DeviceManagerRegistry registry, uint64_t deviceId) {
    if (!registry) return;

    Semaphore_wait(registry->lock);
    int index = findIndexByDeviceId(registry, deviceId);
    if (index >= 0) {
        DeviceManagerPortAllocator_free(registry->ports, registry->entries[index].wsPort);
        free(registry->entries[index].host);
        free(registry->entries[index].ownedAcseAuthPassword);
        /* Swap-remove - order among active devices is never meaningful. */
        registry->entries[index] = registry->entries[registry->count - 1];
        registry->count--;
    }
    Semaphore_post(registry->lock);
}

DeviceManagerError
DeviceManagerRegistry_removeIfRunning(DeviceManagerRegistry registry, uint64_t deviceId,
        OrchestrationHandle* outHandle, uint16_t* outPort, char** outOwnedHost, char** outOwnedAcseAuthPassword) {
    if (!registry) return DEVICE_MANAGER_ERR_INVALID_ARGUMENT;

    Semaphore_wait(registry->lock);

    int index = findIndexByDeviceId(registry, deviceId);
    if (index < 0) {
        Semaphore_post(registry->lock);
        return DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND;
    }

    if (!registry->entries[index].running) {
        Semaphore_post(registry->lock);
        return DEVICE_MANAGER_ERR_START_IN_PROGRESS;
    }

    if (outHandle) *outHandle = registry->entries[index].orchestrationHandle;
    if (outPort) *outPort = registry->entries[index].wsPort;

    if (outOwnedHost) *outOwnedHost = registry->entries[index].host;
    else free(registry->entries[index].host);

    if (outOwnedAcseAuthPassword) *outOwnedAcseAuthPassword = registry->entries[index].ownedAcseAuthPassword;
    else free(registry->entries[index].ownedAcseAuthPassword);

    /* Swap-remove - order among active devices is never meaningful. */
    registry->entries[index] = registry->entries[registry->count - 1];
    registry->count--;

    Semaphore_post(registry->lock);
    return DEVICE_MANAGER_OK;
}

void
DeviceManagerRegistry_freePort(DeviceManagerRegistry registry, uint16_t port) {
    if (!registry) return;

    Semaphore_wait(registry->lock);
    DeviceManagerPortAllocator_free(registry->ports, port);
    Semaphore_post(registry->lock);
}

bool
DeviceManagerRegistry_findDeviceIdByHost(DeviceManagerRegistry registry, const char* host, int mmsPort,
        uint64_t* outDeviceId) {
    if (!registry || !host || !outDeviceId) return false;

    Semaphore_wait(registry->lock);
    bool found = false;
    for (int i = 0; i < registry->count; i++) {
        if (registry->entries[i].mmsPort == mmsPort && registry->entries[i].host
                && strcmp(registry->entries[i].host, host) == 0) {
            *outDeviceId = registry->entries[i].deviceId;
            found = true;
            break;
        }
    }
    Semaphore_post(registry->lock);

    return found;
}

uint64_t
DeviceManagerRegistry_anyRunningDeviceId(DeviceManagerRegistry registry) {
    if (!registry) return 0;

    Semaphore_wait(registry->lock);
    uint64_t deviceId = 0;
    for (int i = 0; i < registry->count; i++) {
        if (registry->entries[i].running) {
            deviceId = registry->entries[i].deviceId;
            break;
        }
    }
    Semaphore_post(registry->lock);

    return deviceId;
}
