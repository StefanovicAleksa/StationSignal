#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"

static void
freeEntriesUpTo(MmsReportEntry* entries, int builtCount) {
    for (int i = 0; i < builtCount; i++) {
        free(entries[i].reference);
        if (entries[i].value) MmsValue_delete(entries[i].value);
    }
    free(entries);
}

bool
MmsReportClientUseCases_hasRealChangeReason(ReasonForInclusion reason) {
    const int realEventBits = IEC61850_REASON_DATA_CHANGE | IEC61850_REASON_QUALITY_CHANGE
            | IEC61850_REASON_DATA_UPDATE;
    return (reason & realEventBits) != 0;
}

bool
MmsReportClientUseCases_isDuplicateValue(const MmsValue* cached, const MmsValue* newValue) {
    if (!cached) return false;
    return MmsValue_equals((MmsValue*) cached, (MmsValue*) newValue);
}

/* Mutates memberRefCache->lastForwardedValues[slot] in place (deletes the old
 * clone, if any, and clones newValue into its place). NULL-safe/no-op if
 * memberRefCache/lastForwardedValues is absent or slot is out of range - a
 * position with no resolvable cache slot simply never gets its value cached,
 * which is fine since shouldForwardAndUpdateCache also never diff-checks it. */
static void
updateValueDiffCache(MmsReportClientMemberRefCacheEntry* memberRefCache, int slot, const MmsValue* newValue) {
    if (!memberRefCache || !memberRefCache->lastForwardedValues) return;
    if (slot < 0 || slot >= memberRefCache->totalLeafSlots) return;

    if (memberRefCache->lastForwardedValues[slot]) MmsValue_delete(memberRefCache->lastForwardedValues[slot]);
    memberRefCache->lastForwardedValues[slot] = newValue ? MmsValue_clone((MmsValue*) newValue) : NULL;
}

/*
 * The hybrid event filter's single decision point, applied once per
 * (possibly decomposed-leaf) value. slot is the value-diff cache index for
 * this exact wire position, or -1 if no cache slot is available for it (no
 * memberRefCache at all, or this position isn't in it) - in that case there's
 * nothing to diff against, so it always survives, matching the old
 * memberRefCache == NULL passthrough behavior.
 *
 * If reason carries a real-change bit, the server is trusted unconditionally
 * (always forward) - the cache is still updated so a later periodic re-send
 * of this same value is recognized as unchanged. Otherwise (periodic-only or
 * unknown/no-reason), forward only if the value differs from the cache (or
 * the cache slot is still NULL, i.e. nothing has ever been forwarded for this
 * position - the case that lets the startup GI snapshot deliver an initial
 * value once).
 */
static bool
shouldForwardAndUpdateCache(MmsReportClientMemberRefCacheEntry* memberRefCache, int slot,
        ReasonForInclusion reason, const MmsValue* value) {
    if (slot < 0) return true;

    if (!MmsReportClientUseCases_hasRealChangeReason(reason)) {
        MmsValue* cached = memberRefCache->lastForwardedValues[slot];
        if (MmsReportClientUseCases_isDuplicateValue(cached, value)) return false;
    }

    updateValueDiffCache(memberRefCache, slot, value);
    return true;
}

typedef struct {
    MmsReportEntry* entries;
    int count;
    int capacity;
} EntryBuilder;

/* Appends one owned, cloned/duplicated entry. Returns false (entry NOT
 * appended) only on allocation failure, so the caller knows not to also
 * update the value-diff cache for it. */
static bool
appendEntry(EntryBuilder* builder, const MmsValue* value, const char* reference, ReasonForInclusion reason) {
    if (builder->count == builder->capacity) {
        int newCapacity = (builder->capacity == 0) ? 4 : (builder->capacity * 2);
        MmsReportEntry* grown = realloc(builder->entries, sizeof(MmsReportEntry) * (size_t) newCapacity);
        if (!grown) return false;
        builder->entries = grown;
        builder->capacity = newCapacity;
    }

    MmsReportEntry* entry = &builder->entries[builder->count++];
    entry->value = value ? MmsValue_clone((MmsValue*) value) : NULL;
    entry->reference = reference ? MmsReportClientUtils_safeStringDup(reference) : NULL;
    entry->reason = reason;
    return true;
}

/* One not-yet-filtered leaf, gathered by collectCandidates before any
 * forward/drop decision is made - value/reference are borrowed (from
 * dataSetValues, a flattened array, or memberRefCache's own strings), never
 * cloned/duped here (appendEntry does that only for leaves that actually end
 * up forwarded). slot mirrors shouldForwardAndUpdateCache's own -1-means-
 * "no cache slot" convention. */
typedef struct {
    const MmsValue* value;
    const char* reference;
    ReasonForInclusion reason;
    int slot;
} EntryCandidate;

typedef struct {
    EntryCandidate* items;
    int count;
    int capacity;
} CandidateBuilder;

/* Same grow-or-skip-on-OOM posture as appendEntry (a single allocation
 * failure drops one candidate rather than aborting the whole report). */
static void
appendCandidate(CandidateBuilder* builder, const MmsValue* value, const char* reference,
        ReasonForInclusion reason, int slot) {
    if (builder->count == builder->capacity) {
        int newCapacity = (builder->capacity == 0) ? 4 : (builder->capacity * 2);
        EntryCandidate* grown = realloc(builder->items, sizeof(EntryCandidate) * (size_t) newCapacity);
        if (!grown) return;
        builder->items = grown;
        builder->capacity = newCapacity;
    }
    EntryCandidate* c = &builder->items[builder->count++];
    c->value = value;
    c->reference = reference;
    c->reason = reason;
    c->slot = slot;
}

/* Tracks flattened-structure arrays (MmsReportClientUtils_flattenStructure
 * results) so they can be freed once at the very end - candidates now
 * outlive the per-position loop that produces them (see collectCandidates),
 * unlike the old single-pass version which freed each one immediately. */
typedef struct {
    MmsValue*** arrays;
    int count;
    int capacity;
} FlattenedArrayList;

static void
trackFlattenedArray(FlattenedArrayList* list, MmsValue** array) {
    if (list->count == list->capacity) {
        int newCapacity = (list->capacity == 0) ? 4 : (list->capacity * 2);
        MmsValue*** grown = realloc(list->arrays, sizeof(MmsValue**) * (size_t) newCapacity);
        if (!grown) {
            free(array); /* can't track it - free now rather than leak */
            return;
        }
        list->arrays = grown;
        list->capacity = newCapacity;
    }
    list->arrays[list->count++] = array;
}

static void
freeFlattenedArrays(FlattenedArrayList* list) {
    for (int i = 0; i < list->count; i++) free(list->arrays[i]);
    free(list->arrays);
}

/*
 * Gap 4 decomposition, unchanged from the old single-pass version - just
 * deferred: instead of deciding forward/drop inline, every (possibly
 * decomposed-leaf) value is recorded as an EntryCandidate for the group-aware
 * filter below to decide on afterwards. See this function's old doc comment
 * (now on buildEntries) for the decomposition/count-mismatch-fallback rule
 * itself, which is unchanged.
 */
static void
collectCandidates(const MmsValue* dataSetValues, const ReasonForInclusion* reasons,
        const char* const* dataReferences, MmsReportClientMemberRefCacheEntry* memberRefCache,
        int entryCount, CandidateBuilder* candidates, FlattenedArrayList* flattenedArrays) {
    for (int i = 0; i < entryCount; i++) {
        ReasonForInclusion reason = reasons ? reasons[i] : IEC61850_REASON_UNKNOWN;
        MmsValue* rawValue = dataSetValues ? MmsValue_getElement((MmsValue*) dataSetValues, i) : NULL;

        bool hasCacheEntry = memberRefCache && i < memberRefCache->memberCount;
        bool isDecomposed = hasCacheEntry && memberRefCache->memberLeafReferences
                && memberRefCache->memberLeafReferences[i];
        bool hasSlots = hasCacheEntry && memberRefCache->leafSlotOffsets;

        if (isDecomposed) {
            int flattenedCount = 0;
            MmsValue** flattened = MmsReportClientUtils_flattenStructure(rawValue, &flattenedCount);

            if (flattened && flattenedCount == memberRefCache->memberLeafCounts[i]) {
                trackFlattenedArray(flattenedArrays, flattened);
                for (int k = 0; k < flattenedCount; k++) {
                    int slot = hasSlots ? memberRefCache->leafSlotOffsets[i] + k : -1;
                    appendCandidate(candidates, flattened[k], memberRefCache->memberLeafReferences[i][k],
                            reason, slot);
                }
                continue; /* raw position i fully handled via decomposition */
            }
            /* Count mismatch (or flatten failure) - fall through to the
             * non-decomposed path below for this one position. */
            free(flattened);
        }

        int slot = hasSlots ? memberRefCache->leafSlotOffsets[i] : -1;
        const char* ref = (dataReferences && dataReferences[i]) ? dataReferences[i]
                : (hasCacheEntry && memberRefCache->memberReferences) ? memberRefCache->memberReferences[i] : NULL;

        appendCandidate(candidates, rawValue, ref, reason, slot);
    }
}

/* Splits `reference` on its LAST "$" into a prefix (length outPrefixLen) and
 * a daName (outDaName points just past the last "$", into the original
 * string). Mirrors ipc_dispatcher's own IpcDispatcherUseCases_splitReference
 * exactly - duplicated locally rather than shared, since mms_report_client
 * may only reach ipc_dispatcher through its service-layer public header,
 * never its domain layer (same small-snippet-duplication convention this
 * codebase already uses elsewhere, e.g. the ACSE-auth-setup duplication
 * between this feature and scl_bootstrap). Returns false if reference is
 * NULL or has no "$" at all. */
static bool
splitReference(const char* reference, size_t* outPrefixLen, const char** outDaName) {
    if (!reference) return false;
    const char* lastDollar = strrchr(reference, '$');
    if (!lastDollar) return false;
    *outPrefixLen = (size_t) (lastDollar - reference);
    *outDaName = lastDollar + 1;
    return true;
}

/* One "q" candidate's own "$"-prefix - the scope its quality applies to.
 * reference is borrowed (points into one of buildEntries' own candidates). */
typedef struct {
    const char* reference;
    size_t prefixLen;
} GroupAnchor;

/*
 * Resolves which "q" anchor scope `reference` belongs to, by finding the
 * LONGEST anchor that `reference` is genuinely nested under (starts with
 * anchor + "$") - mirrors ipc_dispatcher's own
 * findQualityIndexForValue/IpcDispatcherUseCases_pairQuality ancestor-walk
 * logic, just phrased as "resolve to an anchor" instead of "walk upward one
 * level at a time", since here every candidate (not just values) needs
 * grouping, including the "q" candidates themselves (a "q" reference always
 * matches its own anchor, by construction). A flat attribute (e.g.
 * "...Pos$stVal") matches its DO's own "q" anchor directly; a deeply nested
 * CONSTRUCTED-DA chain (e.g. a CMV's "...PhV$phsA$cVal$mag$f") is NOT
 * "adjacent" to its own quality the way a single last-"$"-strip would
 * require - it matches the "...PhV$phsA" anchor several segments up instead,
 * which is what "longest match" naturally finds even without walking level
 * by level (the DO/SDO-level anchor is simply the longest one that's a true
 * ancestor of the leaf). Returns -1 if no anchor is an ancestor of reference.
 */
static int
resolveGroupAnchor(const char* reference, const GroupAnchor* anchors, int anchorCount) {
    if (!reference) return -1;

    int best = -1;
    size_t bestLen = 0;
    size_t refLen = strlen(reference);

    for (int i = 0; i < anchorCount; i++) {
        size_t len = anchors[i].prefixLen;
        if (refLen <= len) continue;
        if (reference[len] != '$') continue;
        if (strncmp(reference, anchors[i].reference, len) != 0) continue;

        if (best == -1 || len > bestLen) {
            best = i;
            bestLen = len;
        }
    }
    return best;
}

/*
 * Builds the final, flat entry list for one report in three phases:
 *   1. collectCandidates - every (possibly Gap-4-decomposed) leaf across
 *      every raw dataset position, undecided.
 *   2. Per-candidate hybrid filter (shouldForwardAndUpdateCache, unchanged),
 *      then a group-aware pass: every "q" candidate's reference anchors a
 *      group scope (its own "$"-prefix); every candidate (including "q"
 *      itself) resolves to the LONGEST anchor it's nested under
 *      (resolveGroupAnchor) - a flat attribute resolves to its DO's own "q"
 *      directly, a deeply nested CONSTRUCTED-DA chain (e.g. a CMV's
 *      "...cVal$mag$f") resolves to the same anchor several segments up
 *      ("...phsA$q"), not whatever its own last "$"-segment happens to be.
 *      A candidate that didn't individually qualify still forwards if ANY
 *      other candidate resolving to the SAME anchor does. This is what keeps
 *      a value+quality pair (or any other sibling DA under the same
 *      DO/SDO - e.g. a Dbpos's "t"/"stSeld") travelling together - without
 *      it, quality (which rarely changes) gets silently dropped by its own
 *      diff-check on every report after the first, orphaning
 *      ipc_dispatcher's quality-pairing (which only pairs entries present in
 *      the SAME record) - and symmetrically, a genuine quality-only change
 *      would leave a lone `q` with no value sibling, which ipc_dispatcher
 *      also drops outright. A candidate with no resolvable anchor at all
 *      (no "q" present in this record's ancestor chain) is its own
 *      ungroupable singleton - falls back to its own diff-check exactly as
 *      before this fix.
 *   3. Emit - every forwarded candidate's cache slot is (re-)updated (a
 *      no-op if shouldForwardAndUpdateCache already updated it, since the
 *      value is identical either way) and appended to the output.
 */
static MmsReportEntry*
buildEntries(const MmsValue* dataSetValues, const ReasonForInclusion* reasons,
        const char* const* dataReferences,
        MmsReportClientMemberRefCacheEntry* memberRefCache,
        int entryCount, int* outEntryCount) {
    *outEntryCount = 0;
    if (entryCount <= 0) return NULL;

    CandidateBuilder candidates = { NULL, 0, 0 };
    FlattenedArrayList flattenedArrays = { NULL, 0, 0 };
    collectCandidates(dataSetValues, reasons, dataReferences, memberRefCache, entryCount,
            &candidates, &flattenedArrays);

    GroupAnchor* anchors = candidates.count > 0 ? calloc((size_t) candidates.count, sizeof(GroupAnchor)) : NULL;
    int anchorCount = 0;
    if (anchors) {
        for (int i = 0; i < candidates.count; i++) {
            size_t prefixLen;
            const char* daName;
            if (!splitReference(candidates.items[i].reference, &prefixLen, &daName)) continue;
            if (strcmp(daName, "q") != 0) continue;
            anchors[anchorCount].reference = candidates.items[i].reference;
            anchors[anchorCount].prefixLen = prefixLen;
            anchorCount++;
        }
    }

    int* groupAnchorIndex = candidates.count > 0 ? malloc(sizeof(int) * (size_t) candidates.count) : NULL;
    if (groupAnchorIndex) {
        for (int i = 0; i < candidates.count; i++) {
            groupAnchorIndex[i] = resolveGroupAnchor(candidates.items[i].reference, anchors, anchorCount);
        }
    }

    bool* forward = candidates.count > 0 ? calloc((size_t) candidates.count, sizeof(bool)) : NULL;
    if (forward) {
        for (int i = 0; i < candidates.count; i++) {
            EntryCandidate* c = &candidates.items[i];
            forward[i] = shouldForwardAndUpdateCache(memberRefCache, c->slot, c->reason, c->value);
        }

        if (groupAnchorIndex) {
            for (int i = 0; i < candidates.count; i++) {
                if (forward[i] || groupAnchorIndex[i] < 0) continue;
                for (int j = 0; j < candidates.count; j++) {
                    if (j == i || !forward[j]) continue;
                    if (groupAnchorIndex[j] == groupAnchorIndex[i]) {
                        forward[i] = true;
                        break;
                    }
                }
            }
        }
    }
    /* forward == NULL (calloc failure) degrades to "nothing forwarded" for
     * this report - matches this function's existing OOM posture elsewhere
     * (a failed allocation drops data rather than crashing). groupAnchorIndex
     * == NULL (or anchors == NULL) degrades to "no grouping" - every
     * candidate falls back to its own solo diff-check. */

    EntryBuilder builder = { NULL, 0, 0 };
    for (int i = 0; i < candidates.count; i++) {
        if (!forward || !forward[i]) continue;

        EntryCandidate* c = &candidates.items[i];
        updateValueDiffCache(memberRefCache, c->slot, c->value);
        appendEntry(&builder, c->value, c->reference, c->reason);
    }

    free(forward);
    free(groupAnchorIndex);
    free(anchors);
    free(candidates.items);
    freeFlattenedArrays(&flattenedArrays);

    if (builder.count == 0) {
        free(builder.entries);
        return NULL;
    }

    *outEntryCount = builder.count;
    return builder.entries;
}

MmsReportRecord*
MmsReportClientUseCases_buildReportRecord(
        const char* rcbReference,
        bool buffered,
        const char* rptId,
        bool hasEntryId, const MmsValue* entryId,
        bool hasTimestamp, uint64_t timestampMs,
        bool hasSeqNum, uint16_t seqNum,
        const MmsValue* dataSetValues,
        const ReasonForInclusion* reasons,
        const char* const* dataReferences,
        MmsReportClientMemberRefCacheEntry* memberRefCache,
        int entryCount) {
    MmsReportRecord* record = calloc(1, sizeof(MmsReportRecord));
    if (!record) return NULL;

    record->rcbReference = MmsReportClientUtils_safeStringDup(rcbReference);
    record->buffered = buffered;
    record->rptId = MmsReportClientUtils_safeStringDup(rptId);

    record->hasEntryId = hasEntryId;
    if (hasEntryId && entryId) record->entryId = MmsValue_clone((MmsValue*) entryId);

    record->hasTimestamp = hasTimestamp;
    record->timestampMs = timestampMs;

    record->hasSeqNum = hasSeqNum;
    record->seqNum = seqNum;

    int survivingEntryCount = 0;
    record->entries = buildEntries(dataSetValues, reasons, dataReferences, memberRefCache,
            entryCount, &survivingEntryCount);
    record->entryCount = survivingEntryCount;

    return record;
}

void
MmsReportClientUseCases_freeReportRecord(MmsReportRecord* record) {
    if (!record) return;

    freeEntriesUpTo(record->entries, record->entryCount);
    free(record->rcbReference);
    free(record->rptId);
    if (record->entryId) MmsValue_delete(record->entryId);
    free(record);
}

uint32_t
MmsReportClientUseCases_computeNextBackoffDelay(uint32_t currentDelayMs, uint32_t initialMs, uint32_t maxMs) {
    if (currentDelayMs == 0) return initialMs;

    uint64_t doubled = (uint64_t) currentDelayMs * 2;
    if (doubled > maxMs) return maxMs;
    return (uint32_t) doubled;
}

void
MmsReportClientUseCases_destroyMemberRefCacheEntry(void* entry) {
    if (!entry) return;
    MmsReportClientMemberRefCacheEntry* e = (MmsReportClientMemberRefCacheEntry*) entry;
    for (int i = 0; i < e->memberCount; i++) free(e->memberReferences[i]);
    free(e->memberReferences);

    if (e->memberLeafReferences) {
        for (int i = 0; i < e->memberCount; i++) {
            if (!e->memberLeafReferences[i]) continue;
            for (int k = 0; k < e->memberLeafCounts[i]; k++) free(e->memberLeafReferences[i][k]);
            free(e->memberLeafReferences[i]);
        }
        free(e->memberLeafReferences);
    }
    free(e->memberLeafCounts);
    free(e->leafSlotOffsets);

    if (e->lastForwardedValues) {
        for (int i = 0; i < e->totalLeafSlots; i++) {
            if (e->lastForwardedValues[i]) MmsValue_delete(e->lastForwardedValues[i]);
        }
        free(e->lastForwardedValues);
    }

    free(e->rcbReference);
    free(e);
}

static void
freeCrossRcbDedupContent(MmsReportClientCrossRcbDedupCache* cache) {
    free(cache->rcbReference);
    cache->rcbReference = NULL;

    for (int i = 0; i < cache->entryCount; i++) {
        free(cache->entries[i].reference);
        if (cache->entries[i].value) MmsValue_delete(cache->entries[i].value);
    }
    free(cache->entries);
    cache->entries = NULL;
    cache->entryCount = 0;
}

void
MmsReportClientUseCases_destroyCrossRcbDedupCache(MmsReportClientCrossRcbDedupCache* cache) {
    if (!cache) return;
    freeCrossRcbDedupContent(cache);
}

static bool
crossRcbEntriesEqual(const MmsReportClientDedupEntry* cached, int cachedCount,
        const MmsReportEntry* entries, int entryCount) {
    if (cachedCount != entryCount) return false;

    for (int i = 0; i < entryCount; i++) {
        const char* cachedRef = cached[i].reference;
        const char* newRef = entries[i].reference;
        if ((cachedRef == NULL) != (newRef == NULL)) return false;
        if (cachedRef && strcmp(cachedRef, newRef) != 0) return false;

        MmsValue* cachedVal = cached[i].value;
        MmsValue* newVal = entries[i].value;
        if ((cachedVal == NULL) != (newVal == NULL)) return false;
        if (cachedVal && !MmsValue_equals(cachedVal, newVal)) return false;
    }
    return true;
}

static void
replaceCrossRcbDedupContent(MmsReportClientCrossRcbDedupCache* cache,
        const char* rcbReference, const MmsReportEntry* entries, int entryCount) {
    freeCrossRcbDedupContent(cache);

    cache->rcbReference = rcbReference ? MmsReportClientUtils_safeStringDup(rcbReference) : NULL;
    if (entryCount <= 0) return;

    MmsReportClientDedupEntry* copy = calloc((size_t) entryCount, sizeof(MmsReportClientDedupEntry));
    if (!copy) return;

    for (int i = 0; i < entryCount; i++) {
        copy[i].reference = entries[i].reference ? MmsReportClientUtils_safeStringDup(entries[i].reference) : NULL;
        copy[i].value = entries[i].value ? MmsValue_clone(entries[i].value) : NULL;
    }
    cache->entries = copy;
    cache->entryCount = entryCount;
}

bool
MmsReportClientUseCases_shouldForwardAcrossRcb(MmsReportClientCrossRcbDedupCache* cache,
        const char* rcbReference, const MmsReportEntry* entries, int entryCount) {
    if (!cache) return true;

    bool isDuplicate = cache->rcbReference && rcbReference
            && strcmp(cache->rcbReference, rcbReference) != 0
            && crossRcbEntriesEqual(cache->entries, cache->entryCount, entries, entryCount);

    if (!isDuplicate) {
        replaceCrossRcbDedupContent(cache, rcbReference, entries, entryCount);
    }
    return !isDuplicate;
}

void
MmsReportClientUseCases_resetValueDiffCache(MmsReportClientMemberRefCacheEntry* entry) {
    if (!entry || !entry->lastForwardedValues) return;
    for (int i = 0; i < entry->totalLeafSlots; i++) {
        if (entry->lastForwardedValues[i]) {
            MmsValue_delete(entry->lastForwardedValues[i]);
            entry->lastForwardedValues[i] = NULL;
        }
    }
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
MmsReportClientUseCases_buildWireMemberReferences(const char* const* memberReferences, int count) {
    LinkedList result = LinkedList_create();
    if (!memberReferences || count <= 0) return result;

    for (int i = 0; i < count; i++) {
        if (!memberReferences[i]) continue;
        char* wireRef = convertToWireMemberReference(memberReferences[i]);
        if (wireRef) LinkedList_add(result, wireRef);
    }
    return result;
}
