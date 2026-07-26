#include <stdlib.h>
#include <string.h>
#include "features/ied_discovery/utils/ied_discovery_utils.h"

char*
IedDiscoveryUtils_safeStringDup(const char* s) {
    if (!s) return NULL;

    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}
