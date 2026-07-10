#ifndef SCAN_DISPATCHER_USECASES_H_
#define SCAN_DISPATCHER_USECASES_H_

#include <stdint.h>
#include "features/scan_dispatcher/domain/scan_dispatcher_types.h"

/*
 * Pure logic - zero third-party includes, unit-testable directly.
 */

/* Deep-copies host into an owned ScanDeviceFoundEvent. Returns NULL only on
 * allocation failure or a NULL/empty host. Caller owns the result:
 * ScanDispatcherUseCases_freeEvent. */
ScanDeviceFoundEvent*
ScanDispatcherUseCases_assembleEvent(uint64_t scanId, const char* host, int mmsPort, uint64_t discoveredAtMs);

/* Frees an event built above, including its owned host string. NULL-safe. */
void
ScanDispatcherUseCases_freeEvent(ScanDeviceFoundEvent* event);

#endif /* SCAN_DISPATCHER_USECASES_H_ */
