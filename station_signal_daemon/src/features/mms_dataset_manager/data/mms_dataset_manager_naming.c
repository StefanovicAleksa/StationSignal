#define SS_LOG_FEATURE "mms_dataset_manager"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_dataset_manager/data/mms_dataset_manager_naming.h"
#include "features/mms_dataset_manager/utils/mms_dataset_manager_utils.h"
#include "log.h"

char*
MmsDatasetManagerNaming_buildDatasetName(const char* lnReference, bool buffered) {
    if (buffered) {
        size_t len = strlen(lnReference) + strlen("$dyn") + 1;
        char* name = malloc(len);
        if (!name) return NULL;
        snprintf(name, len, "%s$dyn", lnReference);
        for (char* p = name; *p; p++) {
            if (*p == '.') *p = '$';
        }
        /* Logged on every derivation, not just on use: this name is the join
         * key for reuse-on-reconnect (IED_ERROR_OBJECT_EXISTS), for the
         * adopt tier's own-name preference, for the orphan cleanup's strict
         * match, and for the stop-path deletion pass. A single character's
         * difference between what this builds and what the server actually
         * holds silently breaks all four, and was previously invisible in a
         * log capture. */
        /* Says "domain-scoped", not "buffered RCB": this function's `buffered`
         * parameter selects the SCOPE, and an UNBUFFERED target's domain-scoped
         * fallback passes true for it too. The old wording asserted the RCB was
         * buffered and was flatly wrong for every fallback - actively confusing
         * while reading these logs to diagnose exactly that path. */
        SS_LOG_DEBUG("[mms_dataset_manager] derived domain-scoped dataset name '%s' from '%s' (persists past "
                "this connection, '.' folded to '$' to match the server's own wire form)\n",
                name, lnReference);
        return name;
    }

    size_t len = strlen("@dyn_") + strlen(lnReference) + 1;
    char* name = malloc(len);
    if (!name) return NULL;
    snprintf(name, len, "@dyn_%s", lnReference);
    for (char* p = name; *p; p++) {
        if (*p == '/') *p = '_';
    }
    SS_LOG_DEBUG("[mms_dataset_manager] derived association-scoped dataset name '%s' from '%s' (destroyed "
            "automatically when this connection closes, no cleanup tracking needed)\n",
            name, lnReference);
    return name;
}

void
MmsDatasetManagerNaming_rememberDomainScopedName(MmsDatasetManagerHandle handle, const char* datasetName) {
    if (!handle->domainScopedDynamicDatasetNames) {
        handle->domainScopedDynamicDatasetNames = LinkedList_create();
        if (!handle->domainScopedDynamicDatasetNames) return;
    }

    LinkedList element = LinkedList_getNext(handle->domainScopedDynamicDatasetNames);
    while (element) {
        if (strcmp((char*) LinkedList_getData(element), datasetName) == 0) {
            SS_LOG_DEBUG("[mms_dataset_manager] domain-scoped dataset '%s' already tracked for cleanup - "
                    "not re-added\n", datasetName);
            return;
        }
        element = LinkedList_getNext(element);
    }

    LinkedList_add(handle->domainScopedDynamicDatasetNames, MmsDatasetManagerUtils_safeStringDup(datasetName));
    /* This list is the ONLY record of what the graceful stop path has to
     * delete - a name that never lands here permanently consumes the
     * device's dataset quota if the daemon is later killed. Logging the
     * insert makes that bookkeeping auditable from a log capture rather than
     * only observable as quota that mysteriously never comes back. */
    SS_LOG_DEBUG("[mms_dataset_manager] tracking domain-scoped dataset '%s' for cleanup on stop (%d name(s) "
            "tracked so far)\n", datasetName, LinkedList_size(handle->domainScopedDynamicDatasetNames));
}

bool
MmsDatasetManagerNaming_looksLikeOurOwnName(const char* datasetRef, const ReportControlBlockTarget* target) {
    if (!datasetRef || !target) return false;
    if (target->buffered) return false;

    if (target->objectReference) {
        char* expectedChunked = MmsDatasetManagerNaming_buildDatasetName(target->objectReference, target->buffered);
        bool match = expectedChunked && strcmp(datasetRef, expectedChunked) == 0;
        free(expectedChunked);
        if (match) return true;
    }

    return false;
}

bool
MmsDatasetManagerNaming_stringListContains(LinkedList list, const char* value) {
    if (!list || !value) return false;
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        char* entry = (char*) LinkedList_getData(element);
        if (entry && strcmp(entry, value) == 0) return true;
        element = LinkedList_getNext(element);
    }
    return false;
}
