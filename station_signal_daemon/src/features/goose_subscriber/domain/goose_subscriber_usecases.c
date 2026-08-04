#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/goose_subscriber/domain/goose_subscriber_usecases.h"
#include "features/goose_subscriber/utils/goose_subscriber_utils.h"

#define GOOSE_SUBSCRIBER_MIN_LIVENESS_POLL_MS 50
#define GOOSE_SUBSCRIBER_DEFAULT_LIVENESS_POLL_MS 1000
#define GOOSE_SUBSCRIBER_LIVENESS_POLL_TAL_DIVISOR 4

static void
freeEntriesUpTo(GooseSubscriberEntry* entries, int builtCount) {
    for (int i = 0; i < builtCount; i++) {
        if (entries[i].value) MmsValue_delete(entries[i].value);
        free(entries[i].reference);
        if (entries[i].previousValue) MmsValue_delete(entries[i].previousValue);
    }
    free(entries);
}

/*
 * MmsValue_equals (libiec61850) is a raw, byte-exact comparison - correct for
 * most types, but wrong for two that show up constantly in real GOOSE
 * datasets, confirmed against real production hardware (via mms_report_client's
 * identical bug - GOOSE frames carry the same DA types, so this fix mirrors
 * that one exactly; see mms_report_client_usecases.c's own doc comment on its
 * copy of this function for the full explanation):
 *
 * - MMS_UTC_TIME: MmsValue_equals memcmp()s all 8 bytes, but the LAST byte is
 *   a TimeQuality flag, not part of "when did this happen" - it can
 *   legitimately wobble even though the displayed millisecond timestamp is
 *   unchanged. Compared instead via MmsValue_getUtcTimeInMs, matching
 *   ipc_dispatcher's own value codec.
 * - MMS_BIT_STRING (CODEDENUM/Dbpos/Tcmd-style status points): MmsValue_equals
 *   memcmp()s the whole buffer INCLUDING unused padding bits, which real
 *   device firmware is commonly inconsistent about zero-padding across
 *   different frame-generation code paths. Compared instead via
 *   MmsValue_getBitStringAsInteger (plus a size guard), matching
 *   ipc_dispatcher's own value codec.
 *
 * Duplicated rather than shared, per this codebase's established
 * per-feature-data/domain-layer convention (features never reach into each
 * other's domain/data layers, only their own service API header).
 */
static bool
valuesAreSemanticallyEqual(const MmsValue* cached, const MmsValue* newValue) {
    MmsType type = MmsValue_getType((MmsValue*) cached);
    if (type != MmsValue_getType((MmsValue*) newValue)) return false;

    switch (type) {
        case MMS_UTC_TIME:
            return MmsValue_getUtcTimeInMs((MmsValue*) cached) == MmsValue_getUtcTimeInMs((MmsValue*) newValue);
        case MMS_BIT_STRING:
            return MmsValue_getBitStringSize((MmsValue*) cached) == MmsValue_getBitStringSize((MmsValue*) newValue)
                    && MmsValue_getBitStringAsInteger((MmsValue*) cached)
                            == MmsValue_getBitStringAsInteger((MmsValue*) newValue);
        default:
            return MmsValue_equals((MmsValue*) cached, (MmsValue*) newValue);
    }
}

bool
GooseSubscriberUseCases_isDuplicateValue(const MmsValue* cached, const MmsValue* newValue) {
    if (!cached || !newValue) return false;
    return valuesAreSemanticallyEqual(cached, newValue);
}

/* Mutates memberRefCache->lastForwardedValues[slot] in place (deletes the old
 * clone, if any, and clones newValue into its place). NULL-safe/no-op if
 * memberRefCache/lastForwardedValues is absent or slot is out of range - a
 * position with no resolvable cache slot simply never gets its value cached,
 * which is fine since shouldForwardAndUpdateCache also never diff-checks it. */
static void
updateValueDiffCache(GooseSubscriberMemberRefCache* memberRefCache, int slot, const MmsValue* newValue) {
    if (!memberRefCache || !memberRefCache->lastForwardedValues) return;
    if (slot < 0 || slot >= memberRefCache->totalLeafSlots) return;

    if (memberRefCache->lastForwardedValues[slot]) MmsValue_delete(memberRefCache->lastForwardedValues[slot]);
    memberRefCache->lastForwardedValues[slot] = newValue ? MmsValue_clone((MmsValue*) newValue) : NULL;
}

/*
 * The group-aware filter's per-candidate decision point. slot is the
 * value-diff cache index for this exact wire position, or -1 if no cache
 * slot is available for it (no memberRefCache at all, or this position isn't
 * in it) - in that case there's nothing to diff against, so it always
 * survives (and outPreviousValue is left NULL), matching the memberRefCache
 * == NULL passthrough behavior.
 *
 * *outPreviousValue (if non-NULL) is set to an owned clone of whatever was
 * cached for this slot BEFORE this call's own cache update - mirrors
 * mms_report_client's identical out-parameter exactly (see that feature's
 * own shouldForwardAndUpdateCache doc comment for the full rationale,
 * including why this is captured regardless of this call's own forward/drop
 * outcome).
 *
 * Unlike mms_report_client's equivalent, GOOSE has no ReasonForInclusion to
 * trust unconditionally - every candidate is diff-gated: if nothing has ever
 * been cached for this slot yet (cached == NULL), this is either (a) this
 * target's very first-ever valid frame (memberRefCache->everPopulated still
 * false - completely expected, nothing to compare against yet) or (b) an
 * unexpected gap discovered AFTER this target has already been populated
 * once (everPopulated true) - case (b) should be structurally impossible
 * under the current design (the cache is populated once, on the first valid
 * frame, and PRESERVED across every later STALE/INVALID_STATE -> VALID
 * recovery - see GooseSubscriberMemberRefCache's own doc comment - nothing
 * ever resets a slot back to NULL again), so it's logged loudly to stderr on
 * every occurrence, mirroring mms_report_client's identical logging exactly.
 * Either way, the cache is silently seeded but this frame is NEVER forwarded
 * itself for a slot with no prior cached value, mirroring MMS's own GI
 * suppression even though GOOSE has no GI concept of its own. Otherwise
 * (cached != NULL), forward only if the value genuinely differs from the
 * cache - this is what makes a recovery's fresh full snapshot compare
 * against the REAL last-known pre-outage value instead of a wiped-clean one.
 */
static bool
shouldForwardAndUpdateCache(GooseSubscriberMemberRefCache* memberRefCache, int slot, const MmsValue* value,
        const char* reference, const char* goCbRef, MmsValue** outPreviousValue) {
    if (outPreviousValue) *outPreviousValue = NULL;
    if (slot < 0) return true;

    MmsValue* cached = memberRefCache->lastForwardedValues[slot];
    if (outPreviousValue && cached) *outPreviousValue = MmsValue_clone(cached);

    if (!value) {
        /* This wire position carries no value at all in this particular
         * frame - GooseSubscriberEntry.value's own doc comment documents
         * this exact possibility ("NULL only if the element itself was NULL
         * in the source array"). Never crash on it (MmsValue_getType(NULL)
         * would, via isDuplicateValue below), never treat a missing value
         * as a bootstrap seed or a real change, and never overwrite a real
         * cached value with NULL - under the populate-once/preserve-forever
         * design (see GooseSubscriberMemberRefCache's own doc comment),
         * re-nulling a slot here would make a later genuine frame for this
         * same position spuriously trip the everPopulated-gated NULL-slot
         * logging above. Simply leave the cache exactly as it was and
         * don't forward this position for this frame. */
        return false;
    }

    if (!cached) {
        if (memberRefCache->everPopulated) {
            fprintf(stderr,
                    "[goose_subscriber] ERROR: cache slot %d (reference '%s') for target '%s' is unexpectedly "
                    "NULL after this target was already populated once - this should never happen, investigate\n",
                    slot, reference ? reference : "(unknown)", goCbRef ? goCbRef : "(unknown)");
        }
        updateValueDiffCache(memberRefCache, slot, value);
        return false;
    }
    if (GooseSubscriberUseCases_isDuplicateValue(cached, value)) return false;

    updateValueDiffCache(memberRefCache, slot, value);
    return true;
}

typedef struct {
    GooseSubscriberEntry* entries;
    int count;
    int capacity;
} GooseEntryBuilder;

/* Appends one owned, cloned/duplicated entry. Returns false (entry NOT
 * appended) only on allocation failure, so the caller knows not to also
 * update the value-diff cache for it. Takes ownership of (moves, does not
 * re-clone) previousValue - mirrors mms_report_client's appendEntry exactly. */
static bool
appendGooseEntry(GooseEntryBuilder* builder, const MmsValue* value, const char* reference,
        MmsValue* previousValue, IedModelDaSemantic semantic) {
    if (builder->count == builder->capacity) {
        int newCapacity = (builder->capacity == 0) ? 4 : (builder->capacity * 2);
        GooseSubscriberEntry* grown = realloc(builder->entries, sizeof(GooseSubscriberEntry) * (size_t) newCapacity);
        if (!grown) return false;
        builder->entries = grown;
        builder->capacity = newCapacity;
    }

    GooseSubscriberEntry* entry = &builder->entries[builder->count++];
    entry->value = value ? MmsValue_clone((MmsValue*) value) : NULL;
    entry->reference = reference ? GooseSubscriberUtils_safeStringDup(reference) : NULL;
    entry->previousValue = previousValue;
    entry->semantic = semantic;
    return true;
}

/* One not-yet-filtered leaf, gathered by collectCandidates before any
 * forward/drop decision is made - value/reference are borrowed (from
 * dataSetValues, a flattened array, or memberRefCache's own strings), never
 * cloned/duped here (appendGooseEntry does that only for leaves that
 * actually end up forwarded). slot mirrors shouldForwardAndUpdateCache's own
 * -1-means-"no cache slot" convention. previousValue is populated by
 * shouldForwardAndUpdateCache itself in buildEntries' phase 2a, regardless of
 * this candidate's own forward/drop outcome. semantic is resolved from
 * memberRefCache->leafSemantics[slot] at collection time. */
typedef struct {
    const MmsValue* value;
    const char* reference;
    int slot;
    MmsValue* previousValue;
    IedModelDaSemantic semantic;
} EntryCandidate;

typedef struct {
    EntryCandidate* items;
    int count;
    int capacity;
} CandidateBuilder;

/* memberRefCache->leafSemantics is parallel to lastForwardedValues (same
 * slot indexing) - NULL-safe/bounds-checked, degrading to NONE exactly like
 * every other "semantics table unavailable" case. */
static IedModelDaSemantic
lookupSemanticForSlot(GooseSubscriberMemberRefCache* memberRefCache, int slot) {
    if (!memberRefCache || !memberRefCache->leafSemantics) return IED_MODEL_DA_SEMANTIC_NONE;
    if (slot < 0 || slot >= memberRefCache->totalLeafSlots) return IED_MODEL_DA_SEMANTIC_NONE;
    return memberRefCache->leafSemantics[slot];
}

/* Same grow-or-skip-on-OOM posture as appendGooseEntry (a single allocation
 * failure drops one candidate rather than aborting the whole record). */
static void
appendCandidate(CandidateBuilder* builder, const MmsValue* value, const char* reference, int slot,
        IedModelDaSemantic semantic) {
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
    c->slot = slot;
    c->previousValue = NULL; /* set later, by shouldForwardAndUpdateCache in buildEntries' phase 2a */
    c->semantic = semantic;
}

/* Tracks flattened-structure arrays (GooseSubscriberUtils_flattenStructure
 * results) so they can be freed once at the very end - candidates now
 * outlive the per-position loop that produces them (see collectCandidates). */
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
 * Gap 4 decomposition: for each raw dataset position i, if
 * memberRefCache->memberLeafReferences[i] is non-NULL (its FCDA was DO-level
 * - see IedModel_getDataSetMemberLeafReferences's own doc comment), flatten
 * its structured value (GooseSubscriberUtils_flattenStructure) and, if the
 * flattened leaf count matches memberLeafCounts[i] (the wire-order
 * assumption held), expand into that many leaf entries instead of one. A
 * count mismatch falls back to a single non-decomposed entry for that one
 * position, matching pre-Gap-4 behavior, rather than mis-pairing labels to
 * values. Every (possibly decomposed-leaf) value is recorded as an
 * EntryCandidate for the group-aware filter in buildEntries to decide on
 * afterwards, rather than deciding forward/drop inline.
 */
/*
 * Reorders `flattened` (length count) into `outReordered` so
 * outReordered[refIdx] is the value that ACTUALLY belongs at
 * leafReferences[refIdx] - mirrors mms_report_client_usecases.c's identical
 * reorderFlattenedToMatchReferences exactly; see that function's own doc
 * comment for the full real-hardware finding (a device whose
 * GetVariableAccessAttributes type-description order doesn't match its own
 * report/GOOSE encoding order), the Quality(13-bit bitstring)/
 * Timestamp(UTC_TIME)-by-fixed-type pass, the expectedTypes-by-
 * IedModel_dataAttributeTypeMatchesMmsType pass (only accepted when it
 * uniquely identifies one remaining wire candidate - this is what resolves
 * a DPC's "stVal"/"stSeld" correctly, since IEC61850_CODEDENUM only ever
 * matches MMS_BIT_STRING and IEC61850_BOOLEAN only ever matches
 * MMS_BOOLEAN), and the everything-else-positional fallback - GOOSE frames
 * carry the same structured DA types as MMS reports, so this is the same
 * exposure.
 */
static bool
reorderFlattenedToMatchReferences(char* const* leafReferences, MmsValue* const* flattened, int count,
        const DataAttributeType* expectedTypes, MmsValue** outReordered) {
    bool* wireUsed = calloc((size_t) count, sizeof(bool));
    if (!wireUsed) return false;

    bool ok = true;
    for (int refIdx = 0; refIdx < count && ok; refIdx++) {
        const char* ref = leafReferences[refIdx];
        const char* lastDollar = ref ? strrchr(ref, '$') : NULL;
        const char* daName = lastDollar ? lastDollar + 1 : ref;
        outReordered[refIdx] = NULL;

        if (daName && strcmp(daName, "q") == 0) {
            for (int w = 0; w < count; w++) {
                if (wireUsed[w] || !flattened[w]) continue;
                if (MmsValue_getType(flattened[w]) == MMS_BIT_STRING
                        && MmsValue_getBitStringSize(flattened[w]) == 13) {
                    outReordered[refIdx] = flattened[w];
                    wireUsed[w] = true;
                    break;
                }
            }
            if (!outReordered[refIdx]) ok = false;
        } else if (daName && strcmp(daName, "t") == 0) {
            for (int w = 0; w < count; w++) {
                if (wireUsed[w] || !flattened[w]) continue;
                if (MmsValue_getType(flattened[w]) == MMS_UTC_TIME) {
                    outReordered[refIdx] = flattened[w];
                    wireUsed[w] = true;
                    break;
                }
            }
            if (!outReordered[refIdx]) ok = false;
        }
        /* else: matched by expected type below if unambiguous, otherwise
         * deferred to the positional fill-in pass further below */
    }

    if (ok && expectedTypes) {
        for (int refIdx = 0; refIdx < count; refIdx++) {
            if (outReordered[refIdx]) continue;
            DataAttributeType expected = expectedTypes[refIdx];

            int matchIdx = -1;
            int matchCount = 0;
            for (int w = 0; w < count; w++) {
                if (wireUsed[w] || !flattened[w]) continue;
                if (IedModel_dataAttributeTypeMatchesMmsType(expected, MmsValue_getType(flattened[w]))) {
                    matchCount++;
                    matchIdx = w;
                }
            }
            if (matchCount == 1) {
                outReordered[refIdx] = flattened[matchIdx];
                wireUsed[matchIdx] = true;
            }
            /* matchCount == 0 or > 1: genuinely ambiguous or unresolvable by
             * type - leave for the positional pass below, same as before. */
        }
    }

    if (ok) {
        int w = 0;
        for (int refIdx = 0; refIdx < count; refIdx++) {
            if (outReordered[refIdx]) continue;
            while (w < count && wireUsed[w]) w++;
            if (w >= count) { ok = false; break; }
            outReordered[refIdx] = flattened[w];
            wireUsed[w] = true;
            w++;
        }
    }

    free(wireUsed);
    return ok;
}

/*
 * The per-leaf EXPECTED-vs-ACTUAL type cross-check that used to live here
 * (decomposedLeafTypesMatch, via IedModel_dataAttributeTypeMatchesMmsType)
 * as a reject-gate was removed at explicit user request - mirrors the
 * identical removal in mms_report_client_usecases.c, confirmed via
 * real-hardware debug logging to reject genuine decompositions
 * (flattenedCount matched memberLeafCounts[i] exactly, but the type check
 * still failed). memberLeafWireTypes is instead handed to
 * reorderFlattenedToMatchReferences above, which consults it purely to
 * disambiguate the reorder - never to reject the decomposition - fixing a
 * real mislabeling bug (q/stVal swapped on one device, stVal/stSeld swapped
 * on another) instead of re-adding a reject-on-mismatch gate.
 */
static void
collectCandidates(const MmsValue* dataSetValues, GooseSubscriberMemberRefCache* memberRefCache,
        int entryCount, CandidateBuilder* candidates, FlattenedArrayList* flattenedArrays) {
    for (int i = 0; i < entryCount; i++) {
        MmsValue* rawValue = dataSetValues ? MmsValue_getElement((MmsValue*) dataSetValues, i) : NULL;

        bool hasCacheEntry = memberRefCache && i < memberRefCache->memberCount;
        bool isDecomposed = hasCacheEntry && memberRefCache->memberLeafReferences
                && memberRefCache->memberLeafReferences[i];
        bool hasSlots = hasCacheEntry && memberRefCache->leafSlotOffsets;

        if (isDecomposed) {
            int flattenedCount = 0;
            MmsValue** flattened = GooseSubscriberUtils_flattenStructure(rawValue, &flattenedCount);

            if (flattened && flattenedCount == memberRefCache->memberLeafCounts[i]) {
                MmsValue** reordered = malloc(sizeof(MmsValue*) * (size_t) flattenedCount);
                const DataAttributeType* expectedTypes = memberRefCache->memberLeafWireTypes
                        ? memberRefCache->memberLeafWireTypes[i] : NULL;
                bool reorderOk = reordered
                        && reorderFlattenedToMatchReferences(memberRefCache->memberLeafReferences[i], flattened,
                                flattenedCount, expectedTypes, reordered);
                if (reorderOk) {
                    trackFlattenedArray(flattenedArrays, flattened);
                    for (int k = 0; k < flattenedCount; k++) {
                        int slot = hasSlots ? memberRefCache->leafSlotOffsets[i] + k : -1;
                        appendCandidate(candidates, reordered[k], memberRefCache->memberLeafReferences[i][k], slot,
                                lookupSemanticForSlot(memberRefCache, slot));
                    }
                    free(reordered);
                    continue; /* raw position i fully handled via decomposition */
                }
                free(reordered);
            }
            /* Count mismatch, flatten failure, or unresolvable q/t reorder -
             * fall through to the non-decomposed path below for this one
             * position. */
            free(flattened);
        }

        int slot = hasSlots ? memberRefCache->leafSlotOffsets[i] : -1;
        const char* ref = (hasCacheEntry && memberRefCache->memberReferences) ? memberRefCache->memberReferences[i]
                : NULL;

        appendCandidate(candidates, rawValue, ref, slot, lookupSemanticForSlot(memberRefCache, slot));
    }
}

/* Splits `reference` on its LAST "$" into a prefix (length outPrefixLen) and
 * a daName (outDaName points just past the last "$", into the original
 * string). Mirrors mms_report_client_usecases.c's own splitReference (itself
 * mirroring ipc_dispatcher's IpcDispatcherUseCases_splitReference) exactly -
 * duplicated locally rather than shared, per this codebase's small-
 * duplication convention between features. Returns false if reference is
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
 * anchor + "$") - mirrors mms_report_client_usecases.c's own
 * resolveGroupAnchor exactly. A flat attribute (e.g. "...Ind1$stVal") matches
 * its DO's own "q" anchor directly; a deeply nested CONSTRUCTED-DA chain
 * (e.g. a CMV's "...PhV$phsA$cVal$mag$f") matches the "...PhV$phsA" anchor
 * several segments up instead. Returns -1 if no anchor is an ancestor of
 * reference.
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
 * Builds the final, flat entry list for one GOOSE message in three phases,
 * mirroring mms_report_client's buildEntries exactly except for the absence
 * of any ReasonForInclusion-equivalent signal (GOOSE has none - every
 * candidate is diff-gated, never trusted unconditionally):
 *   1. collectCandidates - every (possibly Gap-4-decomposed) leaf across
 *      every raw dataset position, undecided.
 *   2. Per-candidate diff filter (shouldForwardAndUpdateCache), then a
 *      group-aware pass: every "q" candidate's reference anchors a group
 *      scope (its own "$"-prefix); every candidate (including "q" itself)
 *      resolves to the LONGEST anchor it's nested under (resolveGroupAnchor).
 *      A candidate that didn't individually qualify still forwards if ANY
 *      other candidate resolving to the SAME anchor does - this is what
 *      keeps a value+quality pair (or any other sibling DA under the same
 *      DO/SDO) travelling together, in both directions. A candidate with no
 *      resolvable anchor at all is its own ungroupable singleton - falls
 *      back to its own diff-check.
 *   3. Emit - every forwarded candidate's cache slot is (re-)updated (a
 *      no-op if shouldForwardAndUpdateCache already updated it) and
 *      appended to the output.
 */
static GooseSubscriberEntry*
buildEntries(const MmsValue* dataSetValues, GooseSubscriberMemberRefCache* memberRefCache,
        const char* goCbRef, int entryCount, int* outEntryCount) {
    *outEntryCount = 0;
    if (entryCount <= 0) return NULL;

    CandidateBuilder candidates = { NULL, 0, 0 };
    FlattenedArrayList flattenedArrays = { NULL, 0, 0 };
    collectCandidates(dataSetValues, memberRefCache, entryCount, &candidates, &flattenedArrays);

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
            forward[i] = shouldForwardAndUpdateCache(memberRefCache, c->slot, c->value, c->reference, goCbRef,
                    &c->previousValue);
        }

        if (groupAnchorIndex) {
            for (int i = 0; i < candidates.count; i++) {
                if (forward[i] || groupAnchorIndex[i] < 0) continue;
                /* A candidate with no value of its own in this frame has
                 * nothing to drag in - forwarding it anyway would let the
                 * updateValueDiffCache call below overwrite a real cached
                 * value with NULL, the exact hazard shouldForwardAndUpdateCache's
                 * own !value branch already guards against (see
                 * mms_report_client_usecases.c's identical fix - found via a
                 * real EntryID-resumed buffered redelivery on the MMS side,
                 * applied here too since the same structural gap exists). */
                if (!candidates.items[i].value) continue;
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
     * this message. groupAnchorIndex == NULL (or anchors == NULL) degrades
     * to "no grouping" - every candidate falls back to its own solo
     * diff-check. */

    GooseEntryBuilder builder = { NULL, 0, 0 };
    for (int i = 0; i < candidates.count; i++) {
        EntryCandidate* c = &candidates.items[i];

        if (!forward || !forward[i]) {
            /* Not forwarded (bootstrap-suppressed, an unchanged duplicate, or
             * never individually qualified and never dragged in by the
             * group-extension pass) - previousValue was still cloned in
             * phase 2a regardless of this outcome, so it must be freed here
             * rather than leaked. */
            if (c->previousValue) MmsValue_delete(c->previousValue);
            continue;
        }

        updateValueDiffCache(memberRefCache, c->slot, c->value);
        if (!appendGooseEntry(&builder, c->value, c->reference, c->previousValue, c->semantic)) {
            /* appendGooseEntry failed to grow its array (OOM) - it never
             * took ownership of previousValue in that case. */
            if (c->previousValue) MmsValue_delete(c->previousValue);
        }
    }

    free(forward);
    free(groupAnchorIndex);
    free(anchors);
    /* Every candidate above was checked against memberRefCache->everPopulated
     * as it stood BEFORE this frame - only now, having processed the whole
     * frame, do we flip it (a no-op once already true). This is what keeps
     * the true first-ever frame's own from-empty seeding silent while making
     * every frame from here on treat an unexpected NULL slot as a bug to
     * log - mirrors mms_report_client's identical buildEntries exactly. */
    if (memberRefCache && candidates.count > 0) memberRefCache->everPopulated = true;
    free(candidates.items);
    freeFlattenedArrays(&flattenedArrays);

    if (builder.count == 0) {
        free(builder.entries);
        return NULL;
    }

    *outEntryCount = builder.count;
    return builder.entries;
}

GooseSubscriberRecord*
GooseSubscriberUseCases_buildRecord(
        const char* goCbRef, const char* goId, const char* dataSet,
        uint32_t stNum, uint32_t sqNum, uint32_t confRev,
        bool test, bool needsCommission,
        uint32_t timeAllowedToLiveMs, uint64_t timestampMs,
        bool hasVlan, uint16_t vlanId, uint8_t vlanPrio, int32_t appId,
        const uint8_t srcMac[6], const uint8_t dstMac[6],
        const MmsValue* dataSetValues,
        GooseSubscriberMemberRefCache* memberRefCache,
        int entryCount) {
    GooseSubscriberRecord* record = calloc(1, sizeof(GooseSubscriberRecord));
    if (!record) return NULL;

    record->goCbRef = GooseSubscriberUtils_safeStringDup(goCbRef);
    record->goId = GooseSubscriberUtils_safeStringDup(goId);
    record->dataSet = GooseSubscriberUtils_safeStringDup(dataSet);

    record->stNum = stNum;
    record->sqNum = sqNum;
    record->confRev = confRev;
    record->test = test;
    record->needsCommission = needsCommission;
    record->timeAllowedToLiveMs = timeAllowedToLiveMs;
    record->timestampMs = timestampMs;

    record->hasVlan = hasVlan;
    record->vlanId = hasVlan ? vlanId : 0;
    record->vlanPrio = hasVlan ? vlanPrio : 0;
    record->appId = appId;

    if (srcMac) memcpy(record->srcMac, srcMac, 6);
    if (dstMac) memcpy(record->dstMac, dstMac, 6);

    int builtEntryCount = 0;
    record->entries = buildEntries(dataSetValues, memberRefCache, goCbRef, entryCount, &builtEntryCount);
    record->entryCount = builtEntryCount;

    return record;
}

void
GooseSubscriberUseCases_freeRecord(GooseSubscriberRecord* record) {
    if (!record) return;

    freeEntriesUpTo(record->entries, record->entryCount);
    free(record->goCbRef);
    free(record->goId);
    free(record->dataSet);
    free(record);
}

bool
GooseSubscriberUseCases_detectStatusTransition(bool wasValid, bool isValid, GooseSubscriberStatus* outStatus) {
    if (wasValid == isValid) return false;

    if (outStatus) {
        /* Caller (the liveness thread, which holds the live GooseSubscriber)
         * refines STALE vs INVALID_STATE via GooseSubscriber_getParseError()
         * when isValid goes false - this pure function can't reach that. */
        *outStatus = isValid ? GOOSE_SUBSCRIBER_STATUS_VALID : GOOSE_SUBSCRIBER_STATUS_STALE;
    }
    return true;
}

uint32_t
GooseSubscriberUseCases_computeLivenessPollIntervalMs(uint32_t configuredMs, int32_t minTalMs) {
    if (configuredMs > 0) return configuredMs;

    if (minTalMs <= 0) return GOOSE_SUBSCRIBER_DEFAULT_LIVENESS_POLL_MS;

    uint32_t derived = (uint32_t) minTalMs / GOOSE_SUBSCRIBER_LIVENESS_POLL_TAL_DIVISOR;
    return derived < GOOSE_SUBSCRIBER_MIN_LIVENESS_POLL_MS ? GOOSE_SUBSCRIBER_MIN_LIVENESS_POLL_MS : derived;
}

bool
GooseSubscriberUseCases_isDuplicateStNum(bool hasForwardedStNum, uint32_t lastForwardedStNum, uint32_t newStNum) {
    return hasForwardedStNum && (lastForwardedStNum == newStNum);
}

static void
freeRecentForwardSlot(GooseSubscriberRecentForwardRecord* slot) {
    free(slot->goCbRef);
    slot->goCbRef = NULL;
    slot->timestampMs = 0;

    for (int i = 0; i < slot->entryCount; i++) {
        free(slot->entries[i].reference);
        if (slot->entries[i].value) MmsValue_delete(slot->entries[i].value);
    }
    free(slot->entries);
    slot->entries = NULL;
    slot->entryCount = 0;
}

void
GooseSubscriberUseCases_destroyRecentForwardCache(GooseSubscriberRecentForwardCache* cache) {
    if (!cache) return;
    for (int i = 0; i < cache->count; i++) {
        freeRecentForwardSlot(&cache->history[i]);
    }
    cache->count = 0;
    cache->nextSlot = 0;
}

static bool
crossTargetEntriesEqual(const GooseSubscriberDedupEntry* cached, int cachedCount,
        const GooseSubscriberEntry* entries, int entryCount) {
    if (cachedCount != entryCount) return false;

    for (int i = 0; i < entryCount; i++) {
        const char* cachedRef = cached[i].reference;
        const char* newRef = entries[i].reference;
        if ((cachedRef == NULL) != (newRef == NULL)) return false;
        if (cachedRef && strcmp(cachedRef, newRef) != 0) return false;

        MmsValue* cachedVal = cached[i].value;
        MmsValue* newVal = entries[i].value;
        if ((cachedVal == NULL) != (newVal == NULL)) return false;
        if (cachedVal && !valuesAreSemanticallyEqual(cachedVal, newVal)) return false;
    }
    return true;
}

static void
fillRecentForwardSlot(GooseSubscriberRecentForwardRecord* slot,
        const char* goCbRef, uint64_t timestampMs, const GooseSubscriberEntry* entries, int entryCount) {
    slot->goCbRef = goCbRef ? GooseSubscriberUtils_safeStringDup(goCbRef) : NULL;
    slot->timestampMs = timestampMs;
    slot->entries = NULL;
    slot->entryCount = 0;
    if (entryCount <= 0) return;

    GooseSubscriberDedupEntry* copy = calloc((size_t) entryCount, sizeof(GooseSubscriberDedupEntry));
    if (!copy) return;

    for (int i = 0; i < entryCount; i++) {
        copy[i].reference = entries[i].reference ? GooseSubscriberUtils_safeStringDup(entries[i].reference) : NULL;
        copy[i].value = entries[i].value ? MmsValue_clone(entries[i].value) : NULL;
    }
    slot->entries = copy;
    slot->entryCount = entryCount;
}

bool
GooseSubscriberUseCases_shouldForwardRecent(GooseSubscriberRecentForwardCache* cache,
        const char* goCbRef, uint64_t timestampMs, const GooseSubscriberEntry* entries, int entryCount) {
    if (!cache) return true;

    for (int i = 0; i < cache->count; i++) {
        GooseSubscriberRecentForwardRecord* slot = &cache->history[i];
        if (slot->timestampMs == timestampMs
                && crossTargetEntriesEqual(slot->entries, slot->entryCount, entries, entryCount)) {
            return false;
        }
    }

    GooseSubscriberRecentForwardRecord* writeSlot = &cache->history[cache->nextSlot];
    if (cache->count == GOOSE_SUBSCRIBER_RECENT_FORWARD_CAPACITY) {
        freeRecentForwardSlot(writeSlot);
    }
    fillRecentForwardSlot(writeSlot, goCbRef, timestampMs, entries, entryCount);

    cache->nextSlot = (cache->nextSlot + 1) % GOOSE_SUBSCRIBER_RECENT_FORWARD_CAPACITY;
    if (cache->count < GOOSE_SUBSCRIBER_RECENT_FORWARD_CAPACITY) cache->count++;

    return true;
}
