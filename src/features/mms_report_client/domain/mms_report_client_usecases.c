#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal_time.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"

/* Temporary diagnostic aid (see CLAUDE.md's ied_model_online_loader bullet /
 * the plan this landed under) - see mms_report_client_api.c's identical
 * helper's own comment for the full reasoning (duplicated here per this
 * codebase's own cross-feature convention). This is the report-time half of
 * the same investigation: mms_report_client_api.c's own debug log tells
 * whether decomposition METADATA exists (memberLeafCounts[i] > 0); this one
 * tells whether a report that HAS that metadata still gets rejected here,
 * at the point where the wire value is actually flattened and zipped. */
static void
appendDebugLog(const char* path, const char* text) {
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%llu] %s\n", (unsigned long long) Hal_getTimeInMs(), text);
    fclose(f);
}

#define MMS_REPORT_CLIENT_DECOMPOSE_DEBUG_LOG_PATH "/tmp/ied_reporter_debug_decompose.log"

static void
freeEntriesUpTo(MmsReportEntry* entries, int builtCount) {
    for (int i = 0; i < builtCount; i++) {
        free(entries[i].reference);
        if (entries[i].value) MmsValue_delete(entries[i].value);
        if (entries[i].previousValue) MmsValue_delete(entries[i].previousValue);
    }
    free(entries);
}

/*
 * MmsValue_equals (libiec61850) is a raw, byte-exact comparison - correct for
 * most types, but wrong for two that show up constantly in real report
 * datasets, confirmed against real production hardware:
 *
 * - MMS_UTC_TIME: MmsValue_equals memcmp()s all 8 bytes, but per IEC 61850
 *   the LAST byte is a TimeQuality flag (leap-second-known/clock-failure/
 *   clock-not-synchronized/accuracy), not part of "when did this happen" -
 *   it can legitimately wobble (a device's clock-sync state settling right
 *   after a reconnect) even though the displayed millisecond timestamp is
 *   unchanged. Compared instead via MmsValue_getUtcTimeInMs - the same
 *   accessor ipc_dispatcher's own value codec already uses to render this
 *   type, so "same JSON output" now correctly implies "same by this filter."
 * - MMS_BIT_STRING (the wire encoding for CODEDENUM/Dbpos/Tcmd-style status
 *   points, see ipc_dispatcher's own value codec): MmsValue_equals memcmp()s
 *   the whole underlying buffer, INCLUDING unused padding bits in the last
 *   byte. Real device firmware is commonly inconsistent about zero-padding
 *   those across different report-generation code paths (e.g. a GI-triggered
 *   read vs. a live-change report) - two values that decode to the identical
 *   integer (and thus render identically) can still fail a raw memcmp.
 *   Compared instead via MmsValue_getBitStringAsInteger (plus a size guard) -
 *   again, the same accessor the value codec already uses to render it.
 *
 * Both gaps let a report that changed nothing get judged "different" and
 * forwarded - observed as value==previousValue duplicates flooding the
 * websocket right at connect (each reconnect/GI cycle gives the comparison a
 * fresh chance to misfire against the freshly-reseeded cache). Every other
 * type falls through to MmsValue_equals unchanged - BOOLEAN/INTEGER/UNSIGNED/
 * FLOAT/STRING already compare semantically correctly, or aren't implicated.
 * goose_subscriber_usecases.c has the identical, independently-duplicated fix
 * (same root cause, same two types, GOOSE frames carry the same DA types).
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
MmsReportClientUseCases_isDuplicateValue(const MmsValue* cached, const MmsValue* newValue) {
    if (!cached || !newValue) return false;
    return valuesAreSemanticallyEqual(cached, newValue);
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
 * The event filter's single decision point, applied once per (possibly
 * decomposed-leaf) value. slot is the value-diff cache index for this exact
 * wire position, or -1 if no cache slot is available for it (no
 * memberRefCache at all, or this position isn't in it) - in that case there's
 * nothing to diff against, so it always survives (and outPreviousValue is
 * left NULL - nothing to report as "previous" either), matching the old
 * memberRefCache == NULL passthrough behavior.
 *
 * *outPreviousValue (if non-NULL) is set to an owned clone of whatever was
 * cached for this slot BEFORE this call's own cache update - the caller
 * reports this as the point's previous value/quality, regardless of whether
 * this call ends up returning true or false (a non-forwarded candidate that
 * later gets dragged into the output by buildEntries' group-extension pass
 * still needs its own previousValue).
 *
 * Always diff-checks against the cache, REGARDLESS of the server's own
 * ReasonForInclusion: if nothing has ever been cached for this slot yet
 * (cached == NULL), this is either (a) the RCB's very first-ever report
 * (memberRefCache->everPopulated still false - completely expected, nothing
 * to compare against yet) or (b) an unexpected gap discovered AFTER this RCB
 * has already been populated once (everPopulated true) - case (b) should be
 * structurally impossible under the current design (the cache is populated
 * once, on first connect, and PRESERVED across every reconnect - see
 * MmsReportClientMemberRefCacheEntry's own doc comment - nothing ever resets
 * a slot back to NULL again), so it's logged loudly to stderr on every
 * occurrence as a bug signal worth investigating on sight, not silently
 * treated the same as case (a). Either way, the cache is silently seeded but
 * this report is NEVER forwarded itself for a slot with no prior cached
 * value. Otherwise (cached != NULL), forward only if the value genuinely
 * differs from the cache - this is what makes a reconnect's fresh GI/
 * redelivered snapshot compare against the REAL last-known pre-disconnect
 * value instead of a wiped-clean one: a genuine change while disconnected
 * now correctly forwards with a real previousValue, an unchanged resend is
 * correctly suppressed.
 *
 * DELIBERATELY does not trust a DATA_CHANGE/QUALITY_CHANGE/DATA_UPDATE
 * reason bit as an unconditional "skip the diff-check" signal, unlike an
 * earlier version of this function. Real-hardware testing against a live IED
 * proved that trust unsafe: the device repeatedly tagged reports as
 * DATA_CHANGE for a value that provably never changed (confirmed via this
 * function's own previousValue output showing previousValue == value on
 * hundreds of consecutive forwarded reports, including - since GI and
 * DATA_CHANGE are independent, combinable ReasonForInclusion bits per
 * third_party/include/iec61850_client.h - GI-triggered snapshots that also
 * carried a real-change bit, bypassing bootstrap-suppression the same way).
 * goose_subscriber's equivalent function has never had this bypass (GOOSE
 * has no ReasonForInclusion concept at all) and was unaffected by this bug,
 * which is what first narrowed it down to this function specifically. Losing
 * the ability to detect a transient A-changes-then-changes-back-to-A
 * round-trip within one buffer interval (the original rationale for trusting
 * the reason bit) is an accepted tradeoff - this codebase now treats a
 * report's "reason" as informational metadata only (still carried on
 * MmsReportEntry.reason), never as a filtering signal.
 */
static bool
shouldForwardAndUpdateCache(MmsReportClientMemberRefCacheEntry* memberRefCache, int slot,
        const MmsValue* value, const char* reference, MmsValue** outPreviousValue) {
    if (outPreviousValue) *outPreviousValue = NULL;
    if (slot < 0) return true;

    MmsValue* cached = memberRefCache->lastForwardedValues[slot];
    if (outPreviousValue && cached) *outPreviousValue = MmsValue_clone(cached);

    if (!value) {
        /* This wire position carries no value at all in this particular
         * report (MmsValue_getElement can legitimately return NULL for some
         * index - GooseSubscriberEntry.value's own doc comment documents
         * the identical possibility on the GOOSE side). Never crash on it
         * (MmsValue_getType(NULL) would, via isDuplicateValue below), never
         * treat a missing value as a bootstrap seed or a real change, and
         * never overwrite a real cached value with NULL - under the
         * populate-once/preserve-forever design (see this struct's own doc
         * comment), re-nulling a slot here would make a later genuine
         * report for this same position spuriously trip the
         * everPopulated-gated NULL-slot logging above. Simply leave the
         * cache exactly as it was and don't forward this position for this
         * report. */
        return false;
    }

    if (!cached) {
        if (memberRefCache->everPopulated) {
            fprintf(stderr,
                    "[mms_report_client] ERROR: cache slot %d (reference '%s') for RCB '%s' is unexpectedly "
                    "NULL after this RCB was already populated once - this should never happen, investigate\n",
                    slot, reference ? reference : "(unknown)",
                    memberRefCache->rcbReference ? memberRefCache->rcbReference : "(unknown)");
        }
        updateValueDiffCache(memberRefCache, slot, value);
        return false;
    }
    if (MmsReportClientUseCases_isDuplicateValue(cached, value)) return false;

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
 * update the value-diff cache for it. Takes ownership of (moves, does not
 * re-clone) previousValue - the caller already cloned it once in
 * shouldForwardAndUpdateCache; on the false-return/OOM path the caller is
 * still responsible for freeing previousValue itself. */
static bool
appendEntry(EntryBuilder* builder, const MmsValue* value, const char* reference, ReasonForInclusion reason,
        MmsValue* previousValue, IedModelDaSemantic semantic) {
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
    entry->previousValue = previousValue;
    entry->semantic = semantic;
    return true;
}

/* One not-yet-filtered leaf, gathered by collectCandidates before any
 * forward/drop decision is made - value/reference are borrowed (from
 * dataSetValues, a flattened array, or memberRefCache's own strings), never
 * cloned/duped here (appendEntry does that only for leaves that actually end
 * up forwarded). slot mirrors shouldForwardAndUpdateCache's own -1-means-
 * "no cache slot" convention. previousValue is populated by
 * shouldForwardAndUpdateCache itself (an owned clone, regardless of this
 * candidate's own forward/drop outcome - see buildEntries' cleanup for why).
 * semantic is resolved from memberRefCache->leafSemantics[slot] at collection
 * time (IED_MODEL_DA_SEMANTIC_NONE if unavailable). */
typedef struct {
    const MmsValue* value;
    const char* reference;
    ReasonForInclusion reason;
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
 * every other "semantics table unavailable" case (see IedModelDaSemantic's
 * own doc comment). */
static IedModelDaSemantic
lookupSemanticForSlot(MmsReportClientMemberRefCacheEntry* memberRefCache, int slot) {
    if (!memberRefCache || !memberRefCache->leafSemantics) return IED_MODEL_DA_SEMANTIC_NONE;
    if (slot < 0 || slot >= memberRefCache->totalLeafSlots) return IED_MODEL_DA_SEMANTIC_NONE;
    return memberRefCache->leafSemantics[slot];
}

/* Same grow-or-skip-on-OOM posture as appendEntry (a single allocation
 * failure drops one candidate rather than aborting the whole report). */
static void
appendCandidate(CandidateBuilder* builder, const MmsValue* value, const char* reference,
        ReasonForInclusion reason, int slot, IedModelDaSemantic semantic) {
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
    c->previousValue = NULL; /* set later, by shouldForwardAndUpdateCache in buildEntries' phase 2a */
    c->semantic = semantic;
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
 * Reorders `flattened` (length count) into `outReordered` (caller-allocated,
 * same length) so outReordered[refIdx] is the value that ACTUALLY belongs at
 * leafReferences[refIdx], even when the wire's real element order doesn't
 * match this daemon's locally-resolved reference order - confirmed via
 * real-hardware debug logging against a device whose GetVariableAccessAttributes
 * type-description order (used to build leafReferences, in
 * ied_model_online_loader_connection.c's buildDataAttributeTreeFromSpec) does
 * NOT match its own report encoding order for the same structured attribute
 * (an "Ind" SPS's stVal/q came out swapped: the bitstring landed on the
 * "$stVal" reference, the boolean on "$q").
 *
 * Two of IEC 61850's standardized common data attributes have a FIXED,
 * unambiguous wire encoding regardless of CDC - Quality ("q") is always a
 * 13-bit MMS_BIT_STRING (Quality_toMmsValue, iec61850_common.c), Timestamp
 * ("t") is always MMS_UTC_TIME - so those two are matched by TYPE (and, for
 * "q", exact bit size, since a CODEDENUM-typed stVal - e.g. a DPC's Dbpos -
 * is also MMS_BIT_STRING but never 13 bits wide) rather than trusting either
 * order. This is NOT "guessing IEC 61850 semantics" (this codebase's own
 * Hard Rule) - Quality's and Timestamp's wire encodings are standardized and
 * identical across every CDC, unlike e.g. a CODEDENUM's Dbpos-vs-Tcmd
 * ambiguity, which is deliberately still never guessed anywhere in this
 * codebase. Whatever's left (the CDC-specific value field(s), e.g. stVal) is
 * assigned positionally among the remaining, unconsumed wire slots, in
 * order - the same trust-the-order assumption as before, just narrowed to
 * only the slot(s) that can't be resolved unambiguously by type.
 *
 * Returns false (caller must not trust this decomposition - fall back to the
 * raw, non-decomposed entry, same as an outright count mismatch) if a "q" or
 * "t" reference exists but no matching-typed, not-yet-claimed wire element
 * was found for it, or if positional fill-in runs out of slots (should be
 * structurally impossible given equal lengths, kept as defense-in-depth).
 */
static bool
reorderFlattenedToMatchReferences(char* const* leafReferences, MmsValue* const* flattened, int count,
        MmsValue** outReordered) {
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
        /* else: deferred to the positional fill-in pass below */
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
 * Gap 4 decomposition, unchanged from the old single-pass version - just
 * deferred: instead of deciding forward/drop inline, every (possibly
 * decomposed-leaf) value is recorded as an EntryCandidate for the group-aware
 * filter below to decide on afterwards. See this function's old doc comment
 * (now on buildEntries) for the decomposition/count-mismatch-fallback rule
 * itself, which is unchanged.
 */
/*
 * The per-leaf EXPECTED-vs-ACTUAL type cross-check that used to live here
 * (decomposedLeafTypesMatch, via IedModel_dataAttributeTypeMatchesMmsType)
 * was removed at explicit user request - confirmed via real-hardware
 * debug logging to reject genuine decompositions (flattenedCount matched
 * memberLeafCounts[i] exactly, but the type check still failed), blocking
 * Gap-4 decomposition outright for a device it should otherwise work on.
 * collectCandidates below now uses reorderFlattenedToMatchReferences instead
 * (see its own doc comment) - not a positional-vs-nothing choice anymore,
 * but positional-with-q/t-corrected-by-type.
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

            /* Per-leaf EXPECTED-vs-ACTUAL type cross-check
             * (decomposedLeafTypesMatch) removed from this gate entirely, at
             * explicit user request - it was rejecting real decompositions
             * on real hardware (flattenedCount matched memberLeafCounts[i]
             * exactly, but the type check still failed), blocking Gap-4
             * decomposition outright for a device it should otherwise work
             * on. memberLeafWireTypes/IedModel_dataAttributeTypeMatchesMmsType
             * are left in place (still populated, just unconsulted here)
             * rather than torn out, since removing them changes nothing
             * behaviorally now. Removing the gate exposed a real
             * mislabeling bug for that same device (q/stVal swapped) -
             * reorderFlattenedToMatchReferences (its own doc comment above)
             * fixes that directly instead of re-adding a reject-on-mismatch
             * gate. */
            if (flattened && flattenedCount == memberRefCache->memberLeafCounts[i]) {
                MmsValue** reordered = malloc(sizeof(MmsValue*) * (size_t) flattenedCount);
                bool reorderOk = reordered
                        && reorderFlattenedToMatchReferences(memberRefCache->memberLeafReferences[i], flattened,
                                flattenedCount, reordered);
                if (reorderOk) {
                    trackFlattenedArray(flattenedArrays, flattened);
                    for (int k = 0; k < flattenedCount; k++) {
                        int slot = hasSlots ? memberRefCache->leafSlotOffsets[i] + k : -1;
                        appendCandidate(candidates, reordered[k], memberRefCache->memberLeafReferences[i][k],
                                reason, slot, lookupSemanticForSlot(memberRefCache, slot));
                    }
                    free(reordered);
                    continue; /* raw position i fully handled via decomposition */
                }
                free(reordered);
            }

            /* Metadata says this member SHOULD decompose (memberLeafCounts[i]
             * > 0, reached this branch at all) but the report-time flatten
             * still didn't match - logging exactly why (flatten failure,
             * count mismatch, or an unresolvable q/t reorder) since this is
             * the other of the two places decomposition can still silently
             * fail even after the online-loader's model-build fix. */
            {
                char line[384];
                snprintf(line, sizeof(line),
                        "member=%d ref=%s REJECTED: flattenedCount=%d expectedCount=%d flattenNonNull=%d",
                        i, memberRefCache->memberLeafReferences[i] && memberRefCache->memberLeafReferences[i][0]
                                ? memberRefCache->memberLeafReferences[i][0] : "(unknown)",
                        flattenedCount, memberRefCache->memberLeafCounts[i], flattened != NULL);
                appendDebugLog(MMS_REPORT_CLIENT_DECOMPOSE_DEBUG_LOG_PATH, line);
            }

            /* Count mismatch, flatten failure, or unresolvable q/t reorder -
             * fall through to the non-decomposed path below for this one
             * position. */
            free(flattened);
        }

        int slot = hasSlots ? memberRefCache->leafSlotOffsets[i] : -1;
        const char* ref = (dataReferences && dataReferences[i]) ? dataReferences[i]
                : (hasCacheEntry && memberRefCache->memberReferences) ? memberRefCache->memberReferences[i] : NULL;

        appendCandidate(candidates, rawValue, ref, reason, slot, lookupSemanticForSlot(memberRefCache, slot));
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
 *   2. Per-candidate value-diff filter (shouldForwardAndUpdateCache - always
 *      diff-checks against the cache now, never trusts a reason bit
 *      unconditionally, see that function's own doc comment), then a
 *      group-aware pass: every "q" candidate's reference anchors a
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
            forward[i] = shouldForwardAndUpdateCache(memberRefCache, c->slot, c->value, c->reference,
                    &c->previousValue);
        }

        if (groupAnchorIndex) {
            for (int i = 0; i < candidates.count; i++) {
                if (forward[i] || groupAnchorIndex[i] < 0) continue;
                /* A candidate with no value of its own in THIS report (e.g. a
                 * buffered/redelivered entry that only captured its sibling's
                 * transition, not this position - confirmed reachable via a
                 * real EntryID-resumed buffered redelivery) has nothing to
                 * drag in: forwarding it anyway would let phase 3's
                 * updateValueDiffCache call below overwrite a real cached
                 * value with NULL, the exact "never overwrite a real cached
                 * value with NULL" hazard shouldForwardAndUpdateCache's own
                 * !value branch already guards against - this closes the
                 * same gap for the group-extension path. */
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
     * this report - matches this function's existing OOM posture elsewhere
     * (a failed allocation drops data rather than crashing). groupAnchorIndex
     * == NULL (or anchors == NULL) degrades to "no grouping" - every
     * candidate falls back to its own solo diff-check. */

    EntryBuilder builder = { NULL, 0, 0 };
    for (int i = 0; i < candidates.count; i++) {
        EntryCandidate* c = &candidates.items[i];

        if (!forward || !forward[i]) {
            /* Not forwarded (bootstrap-suppressed, an unchanged duplicate, or
             * never individually qualified and never dragged in by the group-
             * extension pass) - previousValue was still cloned in phase 2a
             * regardless of this outcome (see shouldForwardAndUpdateCache's
             * own doc comment), so it must be freed here rather than leaked. */
            if (c->previousValue) MmsValue_delete(c->previousValue);
            continue;
        }

        updateValueDiffCache(memberRefCache, c->slot, c->value);
        if (!appendEntry(&builder, c->value, c->reference, c->reason, c->previousValue, c->semantic)) {
            /* appendEntry failed to grow its array (OOM) - it never took
             * ownership of previousValue in that case, so free it ourselves. */
            if (c->previousValue) MmsValue_delete(c->previousValue);
        }
    }

    free(forward);
    free(groupAnchorIndex);
    free(anchors);
    /* Every candidate above was checked against memberRefCache->everPopulated
     * as it stood BEFORE this report - only now, having processed the whole
     * report, do we flip it (a no-op once already true). This is what keeps
     * the true first-ever report's own from-empty seeding silent (see
     * shouldForwardAndUpdateCache's own doc comment) while making every
     * report from here on treat an unexpected NULL slot as a bug to log. */
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

    if (e->memberLeafWireTypes) {
        for (int i = 0; i < e->memberCount; i++) free(e->memberLeafWireTypes[i]);
        free(e->memberLeafWireTypes);
    }

    if (e->lastForwardedValues) {
        for (int i = 0; i < e->totalLeafSlots; i++) {
            if (e->lastForwardedValues[i]) MmsValue_delete(e->lastForwardedValues[i]);
        }
        free(e->lastForwardedValues);
    }
    free(e->leafSemantics);

    if (e->lastEntryId) MmsValue_delete(e->lastEntryId);

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
        if (cachedVal && !valuesAreSemanticallyEqual(cachedVal, newVal)) return false;
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
