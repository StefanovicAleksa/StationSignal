#ifndef DEVICE_MANAGER_PORT_ALLOCATOR_H_
#define DEVICE_MANAGER_PORT_ALLOCATOR_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Simple range + free-list bookkeeping for device_manager's per-device
 * ipc_dispatcher websocket ports - explicit bookkeeping, no bind-and-retry
 * probing (matches this codebase's preference for explicit accounting over
 * speculative I/O). NOT thread-safe on its own - always called under
 * device_manager_registry's own lock, same as scan_orchestration_registry.c
 * folds its own nextScanId counter into its own lock rather than giving it
 * an independent one.
 *
 * Opaque outside this file - nothing else needs field access.
 */
typedef struct sDeviceManagerPortAllocator* DeviceManagerPortAllocator;

/* rangeStart must be <= rangeEnd. Returns NULL on invalid argument or
 * allocation failure. */
DeviceManagerPortAllocator
DeviceManagerPortAllocator_create(uint16_t rangeStart, uint16_t rangeEnd);

/* NULL-safe. */
void
DeviceManagerPortAllocator_destroy(DeviceManagerPortAllocator allocator);

/*
 * Pops the free-list if non-empty (reuse-after-free preferred over growing
 * the never-yet-issued counter, keeps the allocated range compact);
 * otherwise issues the next never-yet-used port in [rangeStart, rangeEnd].
 * Returns false (*outPort left untouched) once every port in the configured
 * range is simultaneously allocated.
 */
bool
DeviceManagerPortAllocator_alloc(DeviceManagerPortAllocator allocator, uint16_t* outPort);

/* Returns a previously-allocated port to the free-list for reuse. NULL-safe. */
void
DeviceManagerPortAllocator_free(DeviceManagerPortAllocator allocator, uint16_t port);

#endif /* DEVICE_MANAGER_PORT_ALLOCATOR_H_ */
