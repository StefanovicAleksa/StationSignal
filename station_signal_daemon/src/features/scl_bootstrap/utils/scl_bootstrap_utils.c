#include <stdlib.h>
#include <string.h>
#include "features/scl_bootstrap/utils/scl_bootstrap_utils.h"

char*
SclBootstrapUtils_safeStringDup(const char* s) {
    if (!s) return NULL;

    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

char*
SclBootstrapUtils_joinPath(const char* parentDir, const char* entryName) {
    if (!entryName) return NULL;
    if (!parentDir || parentDir[0] == '\0') return SclBootstrapUtils_safeStringDup(entryName);

    size_t parentLen = strlen(parentDir);
    size_t entryLen = strlen(entryName);
    char* joined = malloc(parentLen + entryLen + 1);
    if (!joined) return NULL;

    memcpy(joined, parentDir, parentLen);
    memcpy(joined + parentLen, entryName, entryLen + 1);
    return joined;
}
