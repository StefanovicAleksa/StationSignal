#include <stdlib.h>
#include <string.h>
#include "features/mms_report_client/utils/mms_report_client_utils.h"

MmsValue**
MmsReportClientUtils_cloneMmsValueArray(const MmsValue* dataSetValues, int count) {
    if (!dataSetValues || count <= 0) return NULL;

    MmsValue** clones = calloc((size_t) count, sizeof(MmsValue*));
    if (!clones) return NULL;

    for (int i = 0; i < count; i++) {
        MmsValue* element = MmsValue_getElement((MmsValue*) dataSetValues, i);
        clones[i] = element ? MmsValue_clone(element) : NULL;
    }

    return clones;
}

ReasonForInclusion*
MmsReportClientUtils_cloneReasonArray(const ReasonForInclusion* src, int count) {
    if (!src || count <= 0) return NULL;

    ReasonForInclusion* clone = malloc(sizeof(ReasonForInclusion) * (size_t) count);
    if (!clone) return NULL;

    memcpy(clone, src, sizeof(ReasonForInclusion) * (size_t) count);
    return clone;
}

char*
MmsReportClientUtils_safeStringDup(const char* s) {
    if (!s) return NULL;

    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

static void
flattenRecursive(const MmsValue* value, MmsValue*** array, int* count, int* capacity) {
    if (!value) return;

    if (MmsValue_getType((MmsValue*) value) == MMS_STRUCTURE) {
        int elementCount = MmsValue_getArraySize((MmsValue*) value);
        for (int i = 0; i < elementCount; i++) {
            flattenRecursive(MmsValue_getElement((MmsValue*) value, i), array, count, capacity);
        }
        return;
    }

    if (*count == *capacity) {
        int newCapacity = (*capacity == 0) ? 4 : (*capacity * 2);
        MmsValue** grown = realloc(*array, sizeof(MmsValue*) * (size_t) newCapacity);
        /* Allocation failure: drop this leaf rather than crash - the caller's
         * defensive leaf-count-mismatch check (see
         * IedModel_getDataSetMemberLeafReferences's own doc comment) will
         * catch the resulting short count and fall back safely. */
        if (!grown) return;
        *array = grown;
        *capacity = newCapacity;
    }
    (*array)[(*count)++] = (MmsValue*) value;
}

MmsValue**
MmsReportClientUtils_flattenStructure(const MmsValue* value, int* outLeafCount) {
    if (outLeafCount) *outLeafCount = 0;
    if (!value) return NULL;

    MmsValue** array = NULL;
    int count = 0;
    int capacity = 0;
    flattenRecursive(value, &array, &count, &capacity);

    if (outLeafCount) *outLeafCount = count;
    return array;
}
