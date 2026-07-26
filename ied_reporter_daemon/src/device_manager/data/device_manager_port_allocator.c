#include <stdlib.h>
#include "device_manager/data/device_manager_port_allocator.h"

struct sDeviceManagerPortAllocator {
    uint16_t rangeStart;
    uint16_t rangeEnd;
    uint32_t nextUnissued;  /* uint32_t so it can go one past a rangeEnd of UINT16_MAX
                                without wrapping back to 0 */
    uint16_t* freeList;
    int freeCount;
    int freeCapacity;
};

DeviceManagerPortAllocator
DeviceManagerPortAllocator_create(uint16_t rangeStart, uint16_t rangeEnd) {
    if (rangeStart > rangeEnd) return NULL;

    struct sDeviceManagerPortAllocator* allocator = calloc(1, sizeof(struct sDeviceManagerPortAllocator));
    if (!allocator) return NULL;

    allocator->rangeStart = rangeStart;
    allocator->rangeEnd = rangeEnd;
    allocator->nextUnissued = rangeStart;

    return allocator;
}

void
DeviceManagerPortAllocator_destroy(DeviceManagerPortAllocator allocator) {
    if (!allocator) return;

    free(allocator->freeList);
    free(allocator);
}

bool
DeviceManagerPortAllocator_alloc(DeviceManagerPortAllocator allocator, uint16_t* outPort) {
    if (!allocator || !outPort) return false;

    if (allocator->freeCount > 0) {
        *outPort = allocator->freeList[--allocator->freeCount];
        return true;
    }

    if (allocator->nextUnissued > (uint32_t) allocator->rangeEnd) return false;

    *outPort = (uint16_t) allocator->nextUnissued;
    allocator->nextUnissued++;
    return true;
}

void
DeviceManagerPortAllocator_free(DeviceManagerPortAllocator allocator, uint16_t port) {
    if (!allocator) return;

    if (allocator->freeCount >= allocator->freeCapacity) {
        int newCapacity = (allocator->freeCapacity > 0) ? (allocator->freeCapacity * 2) : 8;
        uint16_t* grown = realloc(allocator->freeList, (size_t) newCapacity * sizeof(uint16_t));
        if (!grown) return; /* leaks this one port permanently out of the range - accepted OOM edge case */
        allocator->freeList = grown;
        allocator->freeCapacity = newCapacity;
    }

    allocator->freeList[allocator->freeCount++] = port;
}
