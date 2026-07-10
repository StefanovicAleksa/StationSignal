#include "hal_time.h"
#include "features/scan_dispatcher/data/scan_dispatcher_adapter.h"
#include "features/scan_dispatcher/domain/scan_dispatcher_usecases.h"
#include "features/scan_dispatcher/data/scan_dispatcher_json_writer.h"
#include "features/scan_dispatcher/data/scan_dispatcher_ring_buffer.h"
#include "features/scan_dispatcher/data/scan_dispatcher_ws_server.h"

void
ScanDispatcherAdapter_publishDeviceFound(ScanDispatcherHandle handle, uint64_t scanId, const char* host, int mmsPort) {
    if (!handle || !handle->running) return;

    ScanDeviceFoundEvent* event = ScanDispatcherUseCases_assembleEvent(scanId, host, mmsPort, Hal_getTimeInMs());
    if (!event) return;

    char* json = ScanDispatcherJsonWriter_write(event);
    ScanDispatcherUseCases_freeEvent(event);

    if (json) {
        ScanDispatcherRingBuffer_push(handle->ringBuffer, json); /* ownership -> ring buffer */
        ScanDispatcherWsServer_wake(handle->wsServer);
    }
}
