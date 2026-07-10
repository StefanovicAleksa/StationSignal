#include <stdlib.h>
#include <string.h>
#include "features/scan_dispatcher/domain/scan_dispatcher_usecases.h"

ScanDeviceFoundEvent*
ScanDispatcherUseCases_assembleEvent(uint64_t scanId, const char* host, int mmsPort, uint64_t discoveredAtMs) {
    if (!host || !host[0]) return NULL;

    ScanDeviceFoundEvent* event = calloc(1, sizeof(ScanDeviceFoundEvent));
    if (!event) return NULL;

    event->host = strdup(host);
    if (!event->host) {
        free(event);
        return NULL;
    }

    event->scanId = scanId;
    event->mmsPort = mmsPort;
    event->discoveredAtMs = discoveredAtMs;

    return event;
}

void
ScanDispatcherUseCases_freeEvent(ScanDeviceFoundEvent* event) {
    if (!event) return;

    free(event->host);
    free(event);
}
