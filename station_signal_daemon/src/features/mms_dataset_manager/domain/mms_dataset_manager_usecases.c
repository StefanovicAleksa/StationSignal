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

LinkedList
MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(const char* const* references, int count,
        int maxAttributes) {
    LinkedList chunks = LinkedList_create();
    if (!references || count <= 0 || maxAttributes <= 0) return chunks;

    /* LN-preserving: pack whole LN groups into a chunk while they fit, and
     * only ever split a single LN's own leaves across chunk boundaries when
     * that LN alone exceeds maxAttributes - see this function's own header
     * doc comment. groupReferencesByLn already walks the flat list in
     * contiguous "LD/LN"-prefix runs, which is exactly the grouping this
     * needs, so it's reused directly rather than re-deriving an LN key here. */
    LinkedList lnGroups = MmsDatasetManagerUseCases_groupReferencesByLn(references, count);

    LinkedList currentChunk = NULL;
    int currentChunkSize = 0;

    for (LinkedList groupEl = LinkedList_getNext(lnGroups); groupEl; groupEl = LinkedList_getNext(groupEl)) {
        LinkedList lnGroup = (LinkedList) LinkedList_getData(groupEl);
        int groupSize = LinkedList_size(lnGroup);
        if (groupSize == 0) continue;

        if (groupSize > maxAttributes) {
            /* Oversized LN: flush whatever's pending first, then split this
             * LN's own leaves DO-atomically. Every resulting sub-chunk is
             * reserved for this LN alone - never left open afterward for the
             * next LN to pack into, so a chunk containing any leaf from a
             * split LN never also contains a different LN's leaves. */
            if (currentChunk) {
                LinkedList_add(chunks, currentChunk);
                currentChunk = NULL;
                currentChunkSize = 0;
            }

            char** lnLeaves = calloc((size_t) groupSize, sizeof(char*));
            if (lnLeaves) {
                int li = 0;
                for (LinkedList lnEl = LinkedList_getNext(lnGroup); lnEl; lnEl = LinkedList_getNext(lnEl)) {
                    lnLeaves[li++] = (char*) LinkedList_getData(lnEl);
                }
                LinkedList subChunks = MmsDatasetManagerUseCases_chunkReferencesByDoGroup(
                        (const char* const*) lnLeaves, groupSize, maxAttributes);
                free(lnLeaves);
                /* subChunks' inner lists already own their own duplicated
                 * strings (chunkReferencesByDoGroup dups from lnLeaves,
                 * itself only borrowing lnGroup's strings) - transfer them
                 * directly into `chunks` and destroy only the now-empty
                 * outer container, never Deep. */
                for (LinkedList subEl = LinkedList_getNext(subChunks); subEl; subEl = LinkedList_getNext(subEl)) {
                    LinkedList_add(chunks, (LinkedList) LinkedList_getData(subEl));
                }
                LinkedList_destroyStatic(subChunks);
            }
            continue;
        }

        if (currentChunk && currentChunkSize + groupSize > maxAttributes) {
            LinkedList_add(chunks, currentChunk);
            currentChunk = NULL;
            currentChunkSize = 0;
        }
        if (!currentChunk) currentChunk = LinkedList_create();

        for (LinkedList lnEl = LinkedList_getNext(lnGroup); lnEl; lnEl = LinkedList_getNext(lnEl)) {
            LinkedList_add(currentChunk, MmsDatasetManagerUtils_safeStringDup((char*) LinkedList_getData(lnEl)));
        }
        currentChunkSize += groupSize;
    }

    if (currentChunk) LinkedList_add(chunks, currentChunk);

    for (LinkedList groupCleanup = LinkedList_getNext(lnGroups); groupCleanup;
            groupCleanup = LinkedList_getNext(groupCleanup)) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(groupCleanup), free);
    }
    LinkedList_destroyStatic(lnGroups);

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

/* Local, purely-string linear-scan membership check - deliberately not
 * MmsDatasetManagerNaming_stringListContains (data layer, mms_dataset_manager_naming.h):
 * this domain layer never includes data-layer headers, so a small duplicated
 * helper is preferred over reaching across that boundary, same convention as
 * this file's own stripArrayIndexAnnotation below. */
static bool
containsString(LinkedList list, const char* value) {
    if (!list || !value) return false;
    for (LinkedList el = LinkedList_getNext(list); el; el = LinkedList_getNext(el)) {
        const char* candidate = (const char*) LinkedList_getData(el);
        if (candidate && strcmp(candidate, value) == 0) return true;
    }
    return false;
}

LinkedList
MmsDatasetManagerUseCases_filterOutClaimedLeaves(const char* const* leaves, int count, LinkedList claimedLeaves) {
    LinkedList result = LinkedList_create();
    if (!leaves || count <= 0) return result;

    for (int i = 0; i < count; i++) {
        if (!leaves[i]) continue;
        if (containsString(claimedLeaves, leaves[i])) continue;
        LinkedList_add(result, MmsDatasetManagerUtils_safeStringDup(leaves[i]));
    }
    return result;
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
