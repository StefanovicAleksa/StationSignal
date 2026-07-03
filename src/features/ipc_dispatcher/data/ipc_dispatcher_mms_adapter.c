#include <stdlib.h>
#include "features/ipc_dispatcher/data/ipc_dispatcher_mms_adapter.h"
#include "features/ipc_dispatcher/domain/ipc_dispatcher_usecases.h"
#include "features/ipc_dispatcher/utils/ipc_dispatcher_value_codec.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_json_writer.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ring_buffer.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ws_server.h"

void
IpcDispatcherMmsAdapter_handleReport(IpcDispatcherHandle handle, const MmsReportRecord* record) {
    if (!record) return;
    if (!handle) {
        MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
        return;
    }

    int n = record->entryCount;
    const char** refs = (n > 0) ? malloc((size_t) n * sizeof(char*)) : NULL;
    int* valueIdx = (n > 0) ? malloc((size_t) n * sizeof(int)) : NULL;
    int* qualityIdx = (n > 0) ? malloc((size_t) n * sizeof(int)) : NULL;

    int valueCount = 0;
    if (n > 0 && refs && valueIdx && qualityIdx) {
        for (int i = 0; i < n; i++) refs[i] = record->entries[i].reference;
        valueCount = IpcDispatcherUseCases_pairQuality(refs, n, valueIdx, qualityIdx);
    }

    const char** pointRefs = (valueCount > 0) ? malloc((size_t) valueCount * sizeof(char*)) : NULL;
    IpcScalarValue* pointValues = (valueCount > 0) ? malloc((size_t) valueCount * sizeof(IpcScalarValue)) : NULL;
    bool* pointHasQuality = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(bool)) : NULL;
    IpcQuality* pointQuality = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(IpcQuality)) : NULL;

    int builtCount = 0;
    if (valueCount > 0 && pointRefs && pointValues && pointHasQuality && pointQuality) {
        for (int k = 0; k < valueCount; k++) {
            int vi = valueIdx[k];
            pointRefs[k] = refs[vi];
            pointValues[k] = IpcDispatcherValueCodec_convert(record->entries[vi].value);

            int qi = qualityIdx[k];
            if (qi >= 0) {
                pointHasQuality[k] = IpcDispatcherValueCodec_decodeQuality(record->entries[qi].value, &pointQuality[k]);
            }
        }
        builtCount = valueCount;
    }

    IpcMessage* message = IpcDispatcherUseCases_assembleMessage(
            IPC_SOURCE_MMS_REPORT, record->rcbReference,
            true, record->buffered, record->hasTimestamp, record->timestampMs,
            pointRefs, pointValues, pointHasQuality, pointQuality, builtCount);

    if (message) {
        char* json = IpcDispatcherJsonWriter_write(message);
        IpcDispatcherUseCases_freeMessage(message);
        if (json) {
            IpcDispatcherRingBuffer_push(handle->ringBuffer, json); /* ownership -> ring buffer */
            IpcDispatcherWsServer_wake(handle->wsServer);
        }
    }

    for (int k = 0; k < builtCount; k++) IpcDispatcherValueCodec_freeScalar(&pointValues[k]);
    free(pointValues);
    free(pointHasQuality);
    free(pointQuality);
    free(pointRefs);
    free(refs);
    free(valueIdx);
    free(qualityIdx);

    MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
}
