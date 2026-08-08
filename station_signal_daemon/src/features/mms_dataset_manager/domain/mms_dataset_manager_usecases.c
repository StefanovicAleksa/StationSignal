#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_dataset_manager/domain/mms_dataset_manager_usecases.h"
#include "features/mms_dataset_manager/utils/mms_dataset_manager_utils.h"

bool
MmsDatasetManagerUseCases_isDynamicDatasetBudgetExhausted(int remainingBudget) {
    return remainingBudget == 0;
}

int
MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(int sclMax, int existingDatasetCount) {
    if (sclMax < 0) return -1; /* SCL never declared a cap - stays uncapped, same as today */
    int remaining = sclMax - existingDatasetCount;
    return remaining < 0 ? 0 : remaining;
}

/*
 * Converts one "$"-joined reference (LD/LN$FC$DO[$SDO...]$DA) into
 * IedConnection_createDataSet's required dot/bracket wire form
 * (LD/LN.DO[.SDO...].DA[FC]) - FC moves from the second "$"-segment to a
 * trailing bracket, every segment after it is "."-joined instead of
 * "$"-joined. Returns NULL (malformed - fewer than 3 "$"-segments, or
 * allocation failure) rather than a best-effort partial string; caller skips
 * NULL entries.
 */
static char*
convertToWireMemberReference(const char* ref) {
    char* copy = strdup(ref);
    if (!copy) return NULL;

    char* ldLn = strtok(copy, "$");
    char* fc = ldLn ? strtok(NULL, "$") : NULL;
    if (!ldLn || !fc) {
        free(copy);
        return NULL;
    }

    /* "."-join every remaining "$"-segment (DO[.SDO...].DA). */
    char* joined = NULL;
    char* tok = strtok(NULL, "$");
    while (tok) {
        size_t joinedLen = joined ? strlen(joined) : 0;
        size_t newLen = joinedLen + (joined ? 1 : 0) + strlen(tok) + 1;
        char* next = malloc(newLen);
        if (!next) {
            free(joined);
            free(copy);
            return NULL;
        }
        if (joined) snprintf(next, newLen, "%s.%s", joined, tok);
        else snprintf(next, newLen, "%s", tok);
        free(joined);
        joined = next;
        tok = strtok(NULL, "$");
    }

    if (!joined) {
        /* Only ldLn + fc, nothing after - malformed for this purpose. */
        free(copy);
        return NULL;
    }

    size_t outLen = strlen(ldLn) + 1 + strlen(joined) + 1 + strlen(fc) + 1 + 1;
    char* out = malloc(outLen);
    if (out) snprintf(out, outLen, "%s.%s[%s]", ldLn, joined, fc);

    free(joined);
    free(copy);
    return out;
}

LinkedList
MmsDatasetManagerUseCases_buildWireMemberReferences(const char* const* memberReferences, int count) {
    LinkedList result = LinkedList_create();
    if (!memberReferences || count <= 0) return result;

    for (int i = 0; i < count; i++) {
        if (!memberReferences[i]) continue;
        char* wireRef = convertToWireMemberReference(memberReferences[i]);
        if (wireRef) LinkedList_add(result, wireRef);
    }
    return result;
}

char*
MmsDatasetManagerUseCases_extractDoGroupKey(const char* memberReference) {
    if (!memberReference) return NULL;

    /* "LD/LN$FC$DO[$SDO...]$DA" - the 3rd "$"-segment is the DO name. Walk
     * to the start of that segment (skip past the first two '$'s), then copy
     * up to (but not including) the next '$' or the string end. */
    const char* doStart = strchr(memberReference, '$');
    if (doStart) doStart = strchr(doStart + 1, '$');
    if (!doStart) {
        /* Fewer than 2 '$'s - malformed, treat the whole string as its own
         * singleton group rather than erroring. */
        return MmsDatasetManagerUtils_safeStringDup(memberReference);
    }
    doStart++;

    const char* doEnd = strchr(doStart, '$');
    size_t len = doEnd ? (size_t) (doEnd - doStart) : strlen(doStart);

    char* key = malloc(len + 1);
    if (!key) return NULL;
    memcpy(key, doStart, len);
    key[len] = '\0';
    return key;
}

LinkedList
MmsDatasetManagerUseCases_chunkReferencesByDoGroup(const char* const* references, int count, int maxAttributes) {
    LinkedList chunks = LinkedList_create();
    if (!references || count <= 0 || maxAttributes <= 0) return chunks;

    char** doKeys = calloc((size_t) count, sizeof(char*));
    if (!doKeys) return chunks;
    for (int i = 0; i < count; i++) {
        doKeys[i] = references[i] ? MmsDatasetManagerUseCases_extractDoGroupKey(references[i]) : NULL;
    }

    LinkedList currentChunk = NULL;
    int currentChunkSize = 0;

    int i = 0;
    while (i < count) {
        if (!references[i]) {
            i++;
            continue;
        }

        /* Extent of this DO group: the maximal run of consecutive references
         * sharing the same DO key as references[i]. collectLnLeavesByFc's own
         * DO-then-DA nested walk already emits one DO's leaves contiguously
         * within a given FC pass, so a simple adjacency scan is sufficient -
         * no cross-list DO-key matching is attempted (the same DO name
         * appearing again in a later, non-adjacent FC pass is treated as its
         * own separate group, which is fine - it isn't the same contiguous
         * run this invariant is about). */
        int groupEnd = i + 1;
        while (groupEnd < count && references[groupEnd] && doKeys[groupEnd] && doKeys[i]
                && strcmp(doKeys[groupEnd], doKeys[i]) == 0) {
            groupEnd++;
        }
        int groupSize = groupEnd - i;

        /* Roll over to a new chunk only if the CURRENT chunk already has
         * something in it and the whole incoming group wouldn't fit - never
         * split the group itself, and never roll over an empty chunk (so a
         * lone oversized group still gets exactly one chunk, not an empty one
         * followed by an oversized one). */
        if (currentChunk && currentChunkSize > 0 && currentChunkSize + groupSize > maxAttributes) {
            LinkedList_add(chunks, currentChunk);
            currentChunk = NULL;
            currentChunkSize = 0;
        }
        if (!currentChunk) currentChunk = LinkedList_create();

        for (int j = i; j < groupEnd; j++) {
            LinkedList_add(currentChunk, MmsDatasetManagerUtils_safeStringDup(references[j]));
        }
        currentChunkSize += groupSize;

        i = groupEnd;
    }

    if (currentChunk) LinkedList_add(chunks, currentChunk);

    for (int k = 0; k < count; k++) free(doKeys[k]);
    free(doKeys);

    return chunks;
}

/* Whole-device-safe counterpart of MmsDatasetManagerUseCases_extractDoGroupKey:
 * that function's key is deliberately just the bare DO name (e.g. "Mod"),
 * which is only safe when every reference in the list is already known to
 * belong to the same LN (its only caller, chunkReferencesByDoGroup, is only
 * ever fed one LN's own leaf list). Whole-device clustering feeds a
 * flat list spanning every LN in the model, where two DIFFERENT LNs can
 * easily share a DO name (e.g. "Mod"/"Beh"/"Health" common data, or many
 * same-type LN instances like blkGGIO1..blkGGIO18 all starting with the same
 * first DO name) - if the last DO of one LN and the first DO of the next
 * happened to land adjacent in the flat list with an identical bare-DO key,
 * the greedy packer below would wrongly treat them as one atomic group. This
 * key includes the "LD/LN" prefix too, so it can only ever match within the
 * same LN - safe for cross-LN input, while producing IDENTICAL grouping
 * decisions to the bare-DO-name key for any already-single-LN input (every
 * reference in that case already shares the same LD/LN prefix, so including
 * it in the key changes nothing observable there). */
static char*
extractLnAndDoGroupKey(const char* memberReference) {
    if (!memberReference) return NULL;

    /* "LD/LN$FC$DO[$SDO...]$DA" - key is everything up to and including the
     * DO segment (skip past the FC segment's own '$', then find the end of
     * the DO segment that follows it). */
    const char* fcStart = strchr(memberReference, '$');
    const char* doStart = fcStart ? strchr(fcStart + 1, '$') : NULL;
    if (!doStart) {
        /* Fewer than 2 '$'s - malformed, treat the whole string as its own
         * singleton group rather than erroring, same fallback as
         * MmsDatasetManagerUseCases_extractDoGroupKey. */
        return MmsDatasetManagerUtils_safeStringDup(memberReference);
    }
    doStart++;

    const char* doEnd = strchr(doStart, '$');
    size_t len = doEnd ? (size_t) (doEnd - memberReference) : strlen(memberReference);

    char* key = malloc(len + 1);
    if (!key) return NULL;
    memcpy(key, memberReference, len);
    key[len] = '\0';
    return key;
}

LinkedList
MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(const char* const* references, int count,
        int maxAttributes) {
    LinkedList chunks = LinkedList_create();
    if (!references || count <= 0 || maxAttributes <= 0) return chunks;

    char** groupKeys = calloc((size_t) count, sizeof(char*));
    if (!groupKeys) return chunks;
    for (int i = 0; i < count; i++) {
        groupKeys[i] = references[i] ? extractLnAndDoGroupKey(references[i]) : NULL;
    }

    LinkedList currentChunk = NULL;
    int currentChunkSize = 0;

    int i = 0;
    while (i < count) {
        if (!references[i]) {
            i++;
            continue;
        }

        int groupEnd = i + 1;
        while (groupEnd < count && references[groupEnd] && groupKeys[groupEnd] && groupKeys[i]
                && strcmp(groupKeys[groupEnd], groupKeys[i]) == 0) {
            groupEnd++;
        }
        int groupSize = groupEnd - i;

        if (currentChunk && currentChunkSize > 0 && currentChunkSize + groupSize > maxAttributes) {
            LinkedList_add(chunks, currentChunk);
            currentChunk = NULL;
            currentChunkSize = 0;
        }
        if (!currentChunk) currentChunk = LinkedList_create();

        for (int j = i; j < groupEnd; j++) {
            LinkedList_add(currentChunk, MmsDatasetManagerUtils_safeStringDup(references[j]));
        }
        currentChunkSize += groupSize;

        i = groupEnd;
    }

    if (currentChunk) LinkedList_add(chunks, currentChunk);

    for (int k = 0; k < count; k++) free(groupKeys[k]);
    free(groupKeys);

    return chunks;
}

/* "LD/LN" prefix only (everything before the first '$') - the grouping key
 * for MmsDatasetManagerUseCases_groupReferencesByLn below. */
static char*
extractLnKey(const char* memberReference) {
    if (!memberReference) return NULL;
    const char* fcStart = strchr(memberReference, '$');
    size_t len = fcStart ? (size_t) (fcStart - memberReference) : strlen(memberReference);
    char* key = malloc(len + 1);
    if (!key) return NULL;
    memcpy(key, memberReference, len);
    key[len] = '\0';
    return key;
}

LinkedList
MmsDatasetManagerUseCases_groupReferencesByLn(const char* const* references, int count) {
    LinkedList groups = LinkedList_create();
    if (!references || count <= 0) return groups;

    char** lnKeys = calloc((size_t) count, sizeof(char*));
    if (!lnKeys) return groups;
    for (int i = 0; i < count; i++) {
        lnKeys[i] = references[i] ? extractLnKey(references[i]) : NULL;
    }

    int i = 0;
    while (i < count) {
        if (!references[i]) {
            i++;
            continue;
        }

        int groupEnd = i + 1;
        while (groupEnd < count && references[groupEnd] && lnKeys[groupEnd] && lnKeys[i]
                && strcmp(lnKeys[groupEnd], lnKeys[i]) == 0) {
            groupEnd++;
        }

        LinkedList group = LinkedList_create();
        for (int j = i; j < groupEnd; j++) {
            LinkedList_add(group, MmsDatasetManagerUtils_safeStringDup(references[j]));
        }
        LinkedList_add(groups, group);

        i = groupEnd;
    }

    for (int k = 0; k < count; k++) free(lnKeys[k]);
    free(lnKeys);

    return groups;
}

/* Removes any "(...)" array-index annotation from one dot-separated path
 * segment (e.g. "item(1)component" -> "itemcomponent") - mirrors
 * ied_model_online_loader's own stripArrayIndexAnnotation (a small helper
 * duplicated here rather than shared cross-feature, per this codebase's
 * established convention). This codebase does not model array indices
 * anywhere else (see ied_model's own documented, deliberately deferred
 * DAI/@ix limitation), so preserving the annotation would create a reference
 * shape nothing downstream can consume anyway. */
static char*
stripArrayIndexAnnotation(const char* token) {
    char* result = malloc(strlen(token) + 1);
    if (!result) return NULL;

    char* out = result;
    const char* p = token;
    while (*p) {
        if (*p == '(') {
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
            continue;
        }
        *out++ = *p++;
    }
    *out = '\0';
    return result;
}

char*
MmsDatasetManagerUseCases_convertAcsiRefToMemberReference(const char* acsiRef) {
    if (!acsiRef) return NULL;

    char* copy = strdup(acsiRef);
    if (!copy) return NULL;

    /* Trailing "[FC]" must be the very last thing in the string. */
    char* openBracket = strrchr(copy, '[');
    char* closeBracket = strrchr(copy, ']');
    if (!openBracket || !closeBracket || closeBracket < openBracket || closeBracket[1] != '\0') {
        free(copy);
        return NULL;
    }
    *closeBracket = '\0';
    const char* fc = openBracket + 1;
    *openBracket = '\0';

    /* First "." after the "LD/LN" prefix marks the start of the DO/SDO/DA
     * chain - LD and LN names never themselves contain a "." (only the FCDA
     * path segments after them can, via nested SDOs). */
    char* dot = strchr(copy, '.');
    if (!dot) {
        free(copy);
        return NULL;
    }
    *dot = '\0';

    /* Unlike IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef (which
     * strips the LD half for DataSetEntry_create's sake - a different
     * consumer with a different convention), the memberReferences[]
     * convention this feeds is LD-PRESERVED - matches
     * IedModel_getDataSetMemberReferences's own "LD/LN$FC$DO$DA" output. */
    const char* ldLn = copy;
    if (!strchr(ldLn, '/')) {
        /* Malformed - a real ACSI reference always has "LD/LN". */
        free(copy);
        return NULL;
    }
    char* chain = dot + 1;

    char* joined = NULL;
    char* tok = strtok(chain, ".");
    while (tok) {
        char* clean = stripArrayIndexAnnotation(tok);
        if (!clean) {
            free(joined);
            free(copy);
            return NULL;
        }

        size_t joinedLen = joined ? strlen(joined) : 0;
        size_t newLen = joinedLen + (joined ? 1 : 0) + strlen(clean) + 1;
        char* next = malloc(newLen);
        if (!next) {
            free(clean);
            free(joined);
            free(copy);
            return NULL;
        }
        if (joined) snprintf(next, newLen, "%s$%s", joined, clean);
        else snprintf(next, newLen, "%s", clean);

        free(clean);
        free(joined);
        joined = next;
        tok = strtok(NULL, ".");
    }

    if (!joined) {
        /* Only an "LD/LN" prefix, nothing after - malformed for this purpose. */
        free(copy);
        return NULL;
    }

    size_t outLen = strlen(ldLn) + 1 + strlen(fc) + 1 + strlen(joined) + 1;
    char* out = malloc(outLen);
    if (out) snprintf(out, outLen, "%s$%s$%s", ldLn, fc, joined);

    free(joined);
    free(copy);
    return out;
}
