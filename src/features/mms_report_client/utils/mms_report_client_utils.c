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
