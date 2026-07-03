#include <string.h>
#include <ctype.h>
#include "features/scl_bootstrap/domain/scl_bootstrap_usecases.h"

static const char* const SCL_EXTENSIONS_BY_PRIORITY[] = {
    ".cid", ".icd", ".scd", ".ssd", ".sed"
};
static const int SCL_EXTENSION_COUNT =
    (int) (sizeof(SCL_EXTENSIONS_BY_PRIORITY) / sizeof(SCL_EXTENSIONS_BY_PRIORITY[0]));

static bool
endsWithIgnoreCase(const char* s, const char* suffix) {
    if (!s || !suffix) return false;

    size_t sLen = strlen(s);
    size_t suffixLen = strlen(suffix);
    if (suffixLen > sLen) return false;

    const char* tail = s + (sLen - suffixLen);
    for (size_t i = 0; i < suffixLen; i++) {
        if (tolower((unsigned char) tail[i]) != tolower((unsigned char) suffix[i])) return false;
    }
    return true;
}

bool
SclBootstrapUseCases_isDirectoryEntry(const char* fileName) {
    if (!fileName) return false;

    size_t len = strlen(fileName);
    return len > 0 && fileName[len - 1] == '/';
}

bool
SclBootstrapUseCases_isSclExtension(const char* fileName) {
    return SclBootstrapUseCases_extensionPriority(fileName) >= 0;
}

int
SclBootstrapUseCases_extensionPriority(const char* fileName) {
    if (!fileName || SclBootstrapUseCases_isDirectoryEntry(fileName)) return -1;

    for (int i = 0; i < SCL_EXTENSION_COUNT; i++) {
        if (endsWithIgnoreCase(fileName, SCL_EXTENSIONS_BY_PRIORITY[i])) return i;
    }
    return -1;
}

const char*
SclBootstrapUseCases_pickBestSclFile(LinkedList sclFileCandidates) {
    if (!sclFileCandidates) return NULL;

    const char* best = NULL;
    int bestPriority = SCL_EXTENSION_COUNT;

    LinkedList element = LinkedList_getNext(sclFileCandidates);
    while (element) {
        const char* candidate = (const char*) LinkedList_getData(element);
        int priority = SclBootstrapUseCases_extensionPriority(candidate);

        if (priority >= 0 && (priority < bestPriority ||
                (priority == bestPriority && best && strcmp(candidate, best) < 0))) {
            best = candidate;
            bestPriority = priority;
        }

        element = LinkedList_getNext(element);
    }

    return best;
}

bool
SclBootstrapUseCases_isHostListValid(LinkedList hostList) {
    if (!hostList || LinkedList_size(hostList) == 0) return false;

    LinkedList element = LinkedList_getNext(hostList);
    while (element) {
        const char* host = (const char*) LinkedList_getData(element);
        if (!host || host[0] == '\0') return false;
        element = LinkedList_getNext(element);
    }

    return true;
}
