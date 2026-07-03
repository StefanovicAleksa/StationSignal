#include <stdlib.h>
#include <string.h>
#include "features/ipc_dispatcher/domain/ipc_dispatcher_usecases.h"

/* Pure libc, no third-party dependency - kept local rather than reused from
 * another feature's utils/ so this whole file stays hand-testable with only
 * -lunity (see plan's testing strategy). */
static char*
dupString(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

bool
IpcDispatcherUseCases_splitReference(const char* reference, size_t* outPrefixLen, const char** outDaName) {
    if (!reference) return false;

    const char* lastDollar = strrchr(reference, '$');
    if (!lastDollar) return false;

    *outPrefixLen = (size_t) (lastDollar - reference);
    *outDaName = lastDollar + 1;
    return true;
}

int
IpcDispatcherUseCases_pairQuality(const char* const* references, int count,
        int* outValueIndices, int* outQualityIndexForValue) {
    if (count <= 0) return 0;

    int valueCount = 0;

    for (int i = 0; i < count; i++) {
        size_t prefixLen;
        const char* daName;
        bool parsed = IpcDispatcherUseCases_splitReference(references[i], &prefixLen, &daName);

        /* A "q" entry is only ever a sibling of some other value entry -
         * never emitted itself, whether or not that sibling actually exists. */
        if (parsed && strcmp(daName, "q") == 0) continue;

        int qualityIndex = -1;
        if (parsed) {
            for (int j = 0; j < count; j++) {
                if (j == i) continue;

                size_t otherPrefixLen;
                const char* otherDaName;
                if (!IpcDispatcherUseCases_splitReference(references[j], &otherPrefixLen, &otherDaName)) continue;
                if (strcmp(otherDaName, "q") != 0) continue;
                if (otherPrefixLen != prefixLen) continue;
                if (strncmp(references[j], references[i], prefixLen) != 0) continue;

                qualityIndex = j;
                break;
            }
        }

        outValueIndices[valueCount] = i;
        outQualityIndexForValue[valueCount] = qualityIndex;
        valueCount++;
    }

    return valueCount;
}

static IpcScalarValue
cloneScalarValue(const IpcScalarValue* src) {
    IpcScalarValue copy = *src;
    if (src->type == IPC_SCALAR_STRING || src->type == IPC_SCALAR_RAW) {
        copy.value.str = dupString(src->value.str);
    }
    return copy;
}

IpcMessage*
IpcDispatcherUseCases_assembleMessage(IpcSourceType sourceType, const char* sourceReference,
        bool hasBuffered, bool buffered, bool hasTimestamp, uint64_t timestampMs,
        const char* const* pointReferences, const IpcScalarValue* pointValues,
        const bool* pointHasQuality, const IpcQuality* pointQuality, int pointCount) {
    IpcMessage* message = calloc(1, sizeof(IpcMessage));
    if (!message) return NULL;

    message->sourceType = sourceType;
    message->sourceReference = dupString(sourceReference);
    message->hasBuffered = hasBuffered;
    message->buffered = buffered;
    message->hasTimestamp = hasTimestamp;
    message->timestampMs = timestampMs;

    if (pointCount > 0) {
        message->dataPoints = calloc((size_t) pointCount, sizeof(IpcDataPoint));
        if (!message->dataPoints) {
            free(message->sourceReference);
            free(message);
            return NULL;
        }

        for (int i = 0; i < pointCount; i++) {
            message->dataPoints[i].reference = dupString(pointReferences[i]);
            message->dataPoints[i].value = cloneScalarValue(&pointValues[i]);
            message->dataPoints[i].hasQuality = pointHasQuality && pointHasQuality[i];
            if (message->dataPoints[i].hasQuality) message->dataPoints[i].quality = pointQuality[i];
        }
        message->dataPointCount = pointCount;
    }

    return message;
}

void
IpcDispatcherUseCases_freeMessage(IpcMessage* message) {
    if (!message) return;

    for (int i = 0; i < message->dataPointCount; i++) {
        free(message->dataPoints[i].reference);
        if (message->dataPoints[i].value.type == IPC_SCALAR_STRING || message->dataPoints[i].value.type == IPC_SCALAR_RAW) {
            free(message->dataPoints[i].value.value.str);
        }
    }
    free(message->dataPoints);
    free(message->sourceReference);
    free(message);
}
