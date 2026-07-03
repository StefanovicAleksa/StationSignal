#include <stdlib.h>
#include <string.h>
#include "features/goose_subscriber/utils/goose_subscriber_utils.h"

MmsValue**
GooseSubscriberUtils_cloneMmsValueArray(const MmsValue* dataSetValues, int count) {
    if (!dataSetValues || count <= 0) return NULL;

    MmsValue** clones = calloc((size_t) count, sizeof(MmsValue*));
    if (!clones) return NULL;

    for (int i = 0; i < count; i++) {
        MmsValue* element = MmsValue_getElement((MmsValue*) dataSetValues, i);
        clones[i] = element ? MmsValue_clone(element) : NULL;
    }

    return clones;
}

char*
GooseSubscriberUtils_safeStringDup(const char* s) {
    if (!s) return NULL;

    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}
