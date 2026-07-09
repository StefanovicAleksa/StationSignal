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

/*
 * Finds entry `valueIndex`'s quality sibling by walking its OWN reference's
 * ancestor prefixes from deepest (closest) to shallowest, one "$"-segment at
 * a time, stopping at the first ancestor level that has a "q" sibling among
 * `references`. This is what makes a value's own reference depth irrelevant:
 * a flat attribute (e.g. "...Pos$stVal") finds its "q" one level up
 * ("...Pos$q") on the very first try, exactly like before this fix; a deeply
 * nested CONSTRUCTED-DA chain (e.g. a CMV's "...PhV$phsA$cVal$mag$f") walks
 * past "...PhV$phsA$cVal$mag" and "...PhV$phsA$cVal" (neither has its own
 * "q") before finding "...PhV$phsA$q" several segments up - quality belongs
 * to the whole CDC instance ("phsA"), not to whichever BDA happens to be the
 * terminal leaf of one nested DA within it. Returns -1 if no ancestor level
 * has a "q" sibling at all.
 */
static int
findQualityIndexForValue(const char* const* references, int count, int valueIndex, size_t valuePrefixLen) {
    const char* ref = references[valueIndex];
    size_t searchLen = valuePrefixLen;

    while (true) {
        for (int j = 0; j < count; j++) {
            if (j == valueIndex) continue;

            size_t otherPrefixLen;
            const char* otherDaName;
            if (!IpcDispatcherUseCases_splitReference(references[j], &otherPrefixLen, &otherDaName)) continue;
            if (strcmp(otherDaName, "q") != 0) continue;
            if (otherPrefixLen != searchLen) continue;
            if (strncmp(references[j], ref, searchLen) != 0) continue;

            return j;
        }

        /* Truncate to the next ancestor level: the last '$' strictly before
         * position searchLen within ref. */
        size_t lastDollarPos = 0;
        bool found = false;
        for (size_t k = 0; k < searchLen; k++) {
            if (ref[k] == '$') {
                lastDollarPos = k;
                found = true;
            }
        }
        if (!found) return -1; /* no more ancestors left to try */
        searchLen = lastDollarPos;
    }
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

        int qualityIndex = parsed ? findQualityIndexForValue(references, count, i, prefixLen) : -1;

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
