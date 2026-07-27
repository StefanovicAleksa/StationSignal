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
    bool* pointHasPreviousValue = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(bool)) : NULL;
    IpcScalarValue* pointPreviousValue = (valueCount > 0) ? malloc((size_t) valueCount * sizeof(IpcScalarValue)) : NULL;
    bool* pointHasPreviousQuality = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(bool)) : NULL;
    IpcQuality* pointPreviousQuality = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(IpcQuality)) : NULL;
    bool* pointHasLabel = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(bool)) : NULL;
    const char** pointLabel = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(char*)) : NULL;
    bool* pointHasPreviousLabel = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(bool)) : NULL;
    const char** pointPreviousLabel = (valueCount > 0) ? calloc((size_t) valueCount, sizeof(char*)) : NULL;

    int builtCount = 0;
    if (valueCount > 0 && pointRefs && pointValues && pointHasQuality && pointQuality
            && pointHasPreviousValue && pointPreviousValue && pointHasPreviousQuality && pointPreviousQuality
            && pointHasLabel && pointLabel && pointHasPreviousLabel && pointPreviousLabel) {
        int out = 0;
        for (int k = 0; k < valueCount; k++) {
            int vi = valueIdx[k];
            int qi = qualityIdx[k];

            /* Suppress a value only present because a DIFFERENT sibling DA
             * under the same DO/SDO changed (e.g. a DPC's "t"/"stSeld" riding
             * along with an unrelated "stVal" change) - see
             * IpcDispatcherUseCases_shouldIncludeValuePoint's own doc comment.
             * record->entries[] itself (mms_report_client's group-extension
             * membership) is untouched; this is purely an outbound filter. */
            bool qualityOwnChangeDetected = qi >= 0 && record->entries[qi].ownChangeDetected;
            if (!IpcDispatcherUseCases_shouldIncludeValuePoint(record->entries[vi].ownChangeDetected,
                    qi >= 0, qualityOwnChangeDetected)) {
                continue;
            }

            pointRefs[out] = refs[vi];
            pointValues[out] = IpcDispatcherValueCodec_convert(record->entries[vi].value);

            pointHasPreviousValue[out] = record->entries[vi].previousValue != NULL;
            if (pointHasPreviousValue[out]) {
                pointPreviousValue[out] = IpcDispatcherValueCodec_convert(record->entries[vi].previousValue);
            }

            pointHasLabel[out] = IpcDispatcherValueCodec_decodeDbposLabel(record->entries[vi].semantic,
                    record->entries[vi].value, &pointLabel[out]);
            if (pointHasLabel[out]) {
                /* MmsValue_getBitStringAsInteger (the generic raw-bit read
                 * IpcDispatcherValueCodec_convert used above) and
                 * Dbpos_fromMmsValue disagree on bit order for a genuine
                 * Dbpos bitstring (confirmed empirically: ordinals 1/2 come
                 * out swapped between the two) - once the label is confirmed,
                 * prefer Dbpos_fromMmsValue's own ordinal for the numeric
                 * value too, so "value" and "label" never contradict each
                 * other in the JSON. */
                pointValues[out].value.u64 = (uint64_t) Dbpos_fromMmsValue(record->entries[vi].value);
            }
            if (pointHasPreviousValue[out]) {
                pointHasPreviousLabel[out] = IpcDispatcherValueCodec_decodeDbposLabel(record->entries[vi].semantic,
                        record->entries[vi].previousValue, &pointPreviousLabel[out]);
                if (pointHasPreviousLabel[out]) {
                    pointPreviousValue[out].value.u64 = (uint64_t) Dbpos_fromMmsValue(record->entries[vi].previousValue);
                }
            }

            if (qi >= 0) {
                pointHasQuality[out] = IpcDispatcherValueCodec_decodeQuality(record->entries[qi].value, &pointQuality[out]);
                if (record->entries[qi].previousValue) {
                    pointHasPreviousQuality[out] = IpcDispatcherValueCodec_decodeQuality(
                            record->entries[qi].previousValue, &pointPreviousQuality[out]);
                }
            }

            out++;
        }
        builtCount = out;
    }

    IpcDataPointExtras extras = {
        .pointHasPreviousValue = pointHasPreviousValue,
        .pointPreviousValue = pointPreviousValue,
        .pointHasPreviousQuality = pointHasPreviousQuality,
        .pointPreviousQuality = pointPreviousQuality,
        .pointHasLabel = pointHasLabel,
        .pointLabel = pointLabel,
        .pointHasPreviousLabel = pointHasPreviousLabel,
        .pointPreviousLabel = pointPreviousLabel,
    };

    IpcMessage* message = IpcDispatcherUseCases_assembleMessage(
            IPC_SOURCE_MMS_REPORT, record->rcbReference,
            true, record->buffered, record->hasTimestamp, record->timestampMs,
            pointRefs, pointValues, pointHasQuality, pointQuality, &extras, builtCount);

    if (message) {
        char* json = IpcDispatcherJsonWriter_write(message);
        IpcDispatcherUseCases_freeMessage(message);
        if (json) {
            IpcDispatcherRingBuffer_push(handle->ringBuffer, json); /* ownership -> ring buffer */
            IpcDispatcherWsServer_wake(handle->wsServer);
        }
    }

    for (int k = 0; k < builtCount; k++) {
        IpcDispatcherValueCodec_freeScalar(&pointValues[k]);
        if (pointHasPreviousValue[k]) IpcDispatcherValueCodec_freeScalar(&pointPreviousValue[k]);
    }
    free(pointValues);
    free(pointHasQuality);
    free(pointQuality);
    free(pointHasPreviousValue);
    free(pointPreviousValue);
    free(pointHasPreviousQuality);
    free(pointPreviousQuality);
    free(pointHasLabel);
    free(pointLabel);
    free(pointHasPreviousLabel);
    free(pointPreviousLabel);
    free(pointRefs);
    free(refs);
    free(valueIdx);
    free(qualityIdx);

    MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
}
