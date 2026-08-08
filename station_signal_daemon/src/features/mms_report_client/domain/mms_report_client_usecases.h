#ifndef MMS_REPORT_CLIENT_USECASES_H_
#define MMS_REPORT_CLIENT_USECASES_H_

#include <stdint.h>
#include "features/mms_report_client/domain/mms_report_client_types.h"

/*
 * Pure logic - no ClientReport/IedConnection awareness at all here, that's
 * entirely the data layer's (mms_report_client_report_adapter /
 * mms_report_client_connection) job. Takes plain arguments (strings,
 * MmsValue* arrays, ReasonForInclusion arrays) rather than the opaque
 * ClientReport type specifically so it stays unit-testable: ClientReport has
 * no public constructor, but MmsValue does.
 */

/*
 * Builds a fully-owned, deep-copied MmsReportRecord* from already-extracted
 * report fields. dataSetValues (if non-NULL) must be a MMS_ARRAY/MMS_STRUCTURE
 * MmsValue with at least entryCount elements; reasons/dataReferences (if
 * non-NULL) must each have entryCount elements. Every value/string is cloned/
 * duplicated - none of the inputs are retained by reference. Returns NULL on
 * allocation failure.
 *
 * memberRefCache (nullable) supplies, per raw dataset position: the locally-
 * resolved fallback reference (memberReferences[i], used only when
 * dataReferences[i] is absent), the Gap 4 structure-decomposition metadata
 * (memberLeafReferences[i]/memberLeafCounts[i]), and the value-diff cache
 * (leafSlotOffsets/lastForwardedValues) used by the hybrid event filter - see
 * MmsReportClientMemberRefCacheEntry's own doc comment for the full rule.
 * Passing NULL disables both Gap 4 decomposition and the value-diff filter
 * for this call (every entry is a plain 1:1 passthrough, always forwarded) -
 * used when a report's RCB has no resolvable dataset at all.
 */
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
        int entryCount);

/* Frees a record built by MmsReportClientUseCases_buildReportRecord, including
 * every entry's cloned value/reference and the entries array. NULL-safe. */
void
MmsReportClientUseCases_freeReportRecord(MmsReportRecord* record);

/*
 * Value-diff check for the event filter. cached is the last value actually
 * cached for this exact wire position (NULL means nothing has ever been
 * cached yet - only expected for this RCB's very first-ever report; the
 * cache is populated once and preserved forever after that, never reset on
 * reconnect - see MmsReportClientMemberRefCacheEntry's own doc comment).
 * Returns true (duplicate, should be dropped) only when cached is non-NULL
 * AND bit-for-bit equal (MmsValue_equals) to newValue - a NULL cached value
 * OR a NULL newValue (a wire position can legitimately carry no value in a
 * given report - MmsValue_getElement can return NULL for some index) always
 * returns false (not a duplicate) rather than dereferencing NULL, but that
 * alone does NOT mean
 * "forward it": the caller (shouldForwardAndUpdateCache,
 * mms_report_client_usecases.c) treats a NULL cache as a bootstrap event and
 * seeds the cache WITHOUT forwarding, so the very first-ever snapshot never
 * reaches the caller's report callback, only the first genuine change
 * afterward does (with a real previous value to report, since the cache is
 * now seeded). Every reconnect's own fresh GI/redelivered snapshot instead
 * diffs against the real, preserved last-known value from before the
 * disconnect - not against a wiped-clean cache.
 */
bool
MmsReportClientUseCases_isDuplicateValue(const MmsValue* cached, const MmsValue* newValue);

/*
 * Guards against non-monotonic/duplicate EntryID redelivery WITHIN one RCB's
 * own buffered report stream - orthogonal to (and NOT solved by) the
 * EntryID-resumption-on-enable logic (enableOneTarget,
 * mms_report_client_connection.c), which only uses EntryID to build the
 * RESUME request at (re)enable time, never to validate an already-received
 * report mid-session. Confirmed necessary via a real-hardware capture
 * showing EntryID arriving as 1,4,5,6,7,8,1,9,4,A,5,... within one
 * continuous session (no reconnect involved) - definitively duplicate/stale
 * redelivery, not normal per-IEC-61850-8-1 behavior (EntryID is a
 * server-assigned value that only increases within a buffer epoch).
 *
 * Returns true (stale/duplicate - caller must not process this report) only
 * when both values are non-NULL, both are MMS_OCTET_STRING of EQUAL byte
 * length, and incomingEntryId <= lastSeenEntryId when compared as a
 * fixed-width big-endian unsigned integer (a plain byte-for-byte compare on
 * equal-length buffers already yields this ordering - confirmed against
 * libiec61850's own server-side EntryID encoding, which serializes an 8-byte
 * big-endian counter). Fails OPEN (returns false, "not stale, forward
 * normally") on any NULL/type-mismatch/size-mismatch case - matches this
 * codebase's existing conservative bias (e.g. Gap-4 decomposition falling
 * back to the raw entry on any mismatch): an EntryID shape this function
 * doesn't understand must never cause a legitimate report to be silently
 * dropped.
 */
bool
MmsReportClientUseCases_isEntryIdStale(const MmsValue* incomingEntryId, const MmsValue* lastSeenEntryId);

/*
 * Decides whether enableOneTarget (mms_report_client_connection.c) should
 * request GI on this (re)enable. Confirmed necessary via a real-hardware
 * capture showing that requesting GI alongside an EntryID-based resume on a
 * buffered RCB causes the device to enqueue its GI response as a brand-new
 * entry in the RCB's OWN buffered backlog (fresh, ever-increasing EntryID,
 * but byte-identical stale content) - every reconnect piled one more
 * near-duplicate snapshot into the buffer, and replaying that pile through
 * the single-slot value-diff cache made long-settled values look like they
 * were changing again on every reconnect, well beyond what actually changed
 * during the outage.
 *
 * Returns false (skip GI) only when buffered is true AND hasResumableEntryId
 * is true - a buffered RCB's own EntryID resume already guarantees delivery
 * of everything that happened while disconnected, so GI adds nothing there
 * and is what was polluting the backlog. Returns true (request GI, the
 * pre-existing behavior) in every other case: an unbuffered RCB (no buffer
 * at all - GI is the only way to catch a change made while disconnected), or
 * a buffered RCB with nothing to resume from yet (a genuine first-ever
 * enable, or after an EntryID rejection resets the cache back to NULL) -
 * same full-backlog safety net as before this function existed.
 */
bool
MmsReportClientUseCases_shouldRequestGiOnEnable(bool buffered, bool hasResumableEntryId);

/*
 * True once this connect cycle's own dataset-count budget (a copy of SCL's
 * <Services><DynDataSet max="N"/>, see IedModel_getDynDataSetMax) has been
 * fully spent on new self-created datasets - see getOrCreateDynamicDataset's
 * own doc comment (mms_report_client_connection.c) for where this gates
 * further createDataSet attempts. remainingBudget == -1 (SCL never declared a
 * cap) must never be "exhausted" - only a genuine 0 counts. Extracted as a
 * pure predicate purely so it's directly unit-testable without a live server
 * round-trip.
 */
bool
MmsReportClientUseCases_isDynamicDatasetBudgetExhausted(int remainingBudget);

/*
 * Corrects the naive "just copy SCL's own declared max" budget seeding by
 * accounting for datasets that already exist on the server at the start of
 * this connect cycle (discovered via IedConnection_getLogicalDeviceDataSets,
 * mms_report_client_connection.c's discoverExistingServerDatasets) - a
 * device's REAL remaining budget is smaller than its declared max whenever
 * anything (our own leftover domain-scoped datasets from an ungracefully-
 * terminated prior run, another client's/tool's own datasets, etc.) is
 * already consuming it, something the naive per-cycle reset had zero
 * awareness of. sclMax < 0 (never declared) stays uncapped (-1), regardless
 * of existingDatasetCount - there's nothing to subtract from. Otherwise
 * clamped to a minimum of 0 (existingDatasetCount meeting or exceeding
 * sclMax means no budget for any NEW create this cycle, not a negative
 * number). Pure arithmetic, directly unit-testable.
 */
int
MmsReportClientUseCases_computeInitialDynamicDatasetBudget(int sclMax, int existingDatasetCount);

/* LinkedListValueDeleteFunction-compatible: frees an
 * MmsReportClientMemberRefCacheEntry, including the Gap 4 (memberLeafReferences)
 * cache and the value-diff cache (leafSlotOffsets/lastForwardedValues)
 * alongside the original rcbReference/memberReferences. NULL-safe. */
void
MmsReportClientUseCases_destroyMemberRefCacheEntry(void* entry);

/*
 * Swaps every array-shaped field (memberReferences, memberLeafReferences,
 * memberLeafCounts, leafSlotOffsets, totalLeafSlots, lastForwardedValues,
 * memberLeafWireTypes, leafSemantics, everPopulated, lastEntryId,
 * resolvedDatasetReference) from `fresh` into `entry`, IN PLACE - `entry`'s
 * own pointer identity and rcbReference are left untouched (mms_report_client_connection.c's
 * refreshPulledMemberRefCache/ensureLnFallbackMemberRefCache rely on the
 * entry's pointer never moving: the report-adapter thread's own
 * lookupMemberRefCache walks handle->memberRefCache WITHOUT holding
 * memberRefCacheLock, only field mutations are lock-protected - see that
 * function's own doc comment in mms_report_client_report_adapter.c). Frees
 * entry's OLD field values before adopting fresh's, then frees fresh's own
 * shell (and its now-redundant rcbReference) - fresh must not be used again
 * after this call, NULL-safe on both arguments (no-op if either is NULL).
 *
 * Caller MUST hold handle->memberRefCacheLock for the duration of this call -
 * not taken internally here, since callers need the lock held across the
 * surrounding "is a rebuild even needed" check too (see
 * refreshPulledMemberRefCache's own doc comment for why a shape rebuild
 * deliberately resets the value-diff cache too, rather than preserving it).
 */
void
MmsReportClientUseCases_swapMemberRefCacheEntryShape(MmsReportClientMemberRefCacheEntry* entry,
        MmsReportClientMemberRefCacheEntry* fresh);

/*
 * Resets entry's value-diff cache (lastForwardedValues, everPopulated) back to
 * bootstrap, IN PLACE - unlike MmsReportClientUseCases_swapMemberRefCacheEntryShape,
 * leaves the shape itself (memberReferences, leafSlotOffsets, totalLeafSlots,
 * leafSemantics, resolvedDatasetReference) completely untouched, since this is
 * for the case where the shape hasn't changed but the device's own report
 * state has - a buffered RCB's EntryID getting rejected as
 * IED_ERROR_OBJECT_DOES_NOT_EXIST on (re)enable is the definitive signal for
 * this (mms_report_client_connection.c's enableOneTarget): the device's
 * buffer/EntryID counter was reset (most commonly a real device reboot), so
 * a subsequently recreated dataset - even one with the exact same
 * deterministic name buildDynamicDatasetName always produces, which is why
 * the name-based shape-rebuild check alone does NOT catch this case - holds
 * genuinely fresh values that must not be diffed against stale pre-reset
 * ones. Frees each lastForwardedValues[i] before NULLing it, same as
 * freeMemberRefCacheEntryFields; NULL-safe (no-op if entry or its
 * lastForwardedValues array is NULL).
 *
 * Caller MUST hold handle->memberRefCacheLock for the duration of this call -
 * not taken internally here, same convention as
 * MmsReportClientUseCases_swapMemberRefCacheEntryShape.
 */
void
MmsReportClientUseCases_resetValueDiffCacheToBootstrap(MmsReportClientMemberRefCacheEntry* entry);

/*
 * Cross-RCB duplicate-content suppression - see MmsReportClientCrossRcbDedupCache's
 * own doc comment for the full rationale (redundant RCB instances on the
 * same LN/dataset reporting the same event independently). Compares
 * (rcbReference, entries[0..entryCount)) against cache's own last-forwarded
 * content: if cache has prior content, rcbReference differs from the one
 * that produced it, AND every (reference, value) pair matches positionally,
 * this is a duplicate - returns false (do not forward), leaving cache
 * untouched. Otherwise (nothing cached yet, same rcbReference as before, or
 * genuinely different content) returns true and replaces cache's content
 * with this call's (rcbReference, entries) - so a suppressed duplicate never
 * disturbs the established baseline, but every other case refreshes it.
 * NULL-safe on cache (returns true, i.e. always forward, if cache is NULL -
 * matches this codebase's "no cache means no filtering" convention).
 */
bool
MmsReportClientUseCases_shouldForwardAcrossRcb(MmsReportClientCrossRcbDedupCache* cache,
        const char* rcbReference, const MmsReportEntry* entries, int entryCount);

/* Frees cache's owned rcbReference/entries and resets it back to empty
 * ("nothing forwarded yet"). NULL-safe. Used both internally by
 * MmsReportClientUseCases_shouldForwardAcrossRcb (to replace stale content)
 * and by MmsReportClient_destroy (final cleanup). */
void
MmsReportClientUseCases_destroyCrossRcbDedupCache(MmsReportClientCrossRcbDedupCache* cache);

/*
 * Converts `count` "$"-joined references (LD/LN$FC$DO[$SDO...]$DA - the
 * format IedModel_getReportableAttributeReferencesForLogicalNode/
 * getDataSetMemberReferences already produce) into
 * IedConnection_createDataSet's required dot/bracket dataSetElements format
 * (LD/LN.DO[.SDO...].DA[FC], per iec61850_client.h's documented format for
 * that parameter). Used only for dynamically-created datasets (RCBs whose
 * SCL never declared a datSet) - static, SCL-declared datasets never need
 * this conversion. A malformed input reference (fewer than 3 "$"-segments) is
 * silently skipped, not included in the result - never a partial/best-effort
 * string. Pure string manipulation, no third-party/network calls. Caller owns
 * the returned list and its elements: LinkedList_destroyDeep(list, free).
 */
LinkedList
MmsReportClientUseCases_buildWireMemberReferences(const char* const* memberReferences, int count);

/*
 * Extracts the DO group key (the 3rd "$"-segment: "LD/LN$FC$DO[$SDO...]$DA" -
 * same format as MmsReportClientUseCases_buildWireMemberReferences's own input)
 * from one member reference, for MmsReportClientUseCases_chunkReferencesByDoGroup's
 * own DO-atomic grouping below. A malformed reference (fewer than 3
 * "$"-segments) returns a copy of the whole string as its own singleton
 * group, rather than erroring - this function only groups, it never rejects.
 * Caller owns the returned string (free).
 */
char*
MmsReportClientUseCases_extractDoGroupKey(const char* memberReference);

/*
 * Greedily packs `references` (already scoped to one LN, in their existing
 * FC=ST-then-MX declaration order - see
 * IedModel_getReportableAttributeReferencesForLogicalNode) into DO-atomic
 * chunks of at most maxAttributes members each: one Data Object's own leaves
 * (stVal/q/t, etc.) are never split across two chunks, even if that DO alone
 * exceeds maxAttributes - that DO becomes its own oversized chunk instead,
 * handled downstream exactly like any other tier-3 createDataSet failure, not
 * specially. Used by mms_report_client_connection.c's buildChunkPlan to map
 * an oversized LN's full leaf set onto its own spare RCB instances, one chunk
 * per instance.
 *
 * Strategy is deliberately simple: greedy, order-preserving, DO-atomic - no
 * bin-packing optimization, no reordering to minimize chunk count. This is
 * also the one piece of logic likely reusable as-is by a future no-SCL
 * empirical-discovery follow-up (deferred - see GAP3_DYNAMIC_DATASET_NOTES.md),
 * once that path has discovered a working size limit some other way; it
 * doesn't care where maxAttributes came from.
 *
 * maxAttributes <= 0 or count <= 0 returns an empty list (nothing to chunk).
 * Returns a LinkedList of LinkedList-of-owned-char*, one inner list per chunk
 * in chunk order. Caller owns the outer list AND must
 * LinkedList_destroyDeep(innerList, free) each inner list before
 * LinkedList_destroyStatic(outerList).
 */
LinkedList
MmsReportClientUseCases_chunkReferencesByDoGroup(const char* const* references, int count, int maxAttributes);

/*
 * Whole-device counterpart of MmsReportClientUseCases_chunkReferencesByDoGroup -
 * identical greedy, order-preserving, DO-atomic packing strategy and identical
 * calling convention/ownership rules, but safe for `references` spanning
 * MULTIPLE LNs (e.g. IedModel_getReportableAttributeReferencesForWholeDevice's
 * own output) rather than one LN's own leaf list. The only difference is the
 * internal grouping key: it includes the "LD/LN" prefix, not just the bare DO
 * name, so two different LNs that happen to share a DO name (e.g. "Mod") can
 * never be wrongly merged into one atomic group just because they land
 * adjacent in the flat list - see the .c file's extractLnAndDoGroupKey for
 * the full reasoning. A resulting chunk MAY legitimately span several
 * different (small) LNs' worth of leaves when they fit together under
 * maxAttributes, maximizing device coverage within a tight total
 * dataset-count budget - this is the intended behavior, not a bug.
 */
LinkedList
MmsReportClientUseCases_chunkReferencesAcrossWholeDevice(const char* const* references, int count, int maxAttributes);

/*
 * Groups `references` (spanning multiple LNs, e.g.
 * IedModel_getReportableAttributeReferencesForWholeDevice's own output) into
 * one group per LN, by contiguous "LD/LN"-prefix run - no size cap. Used as
 * whole-device clustering's fallback granularity when SCL's own
 * maxAttributes cap is unknown (chunkReferencesAcrossWholeDevice's own
 * cross-LN bin-packing needs a real size bound to safely combine multiple
 * LNs into one dataset; without one, one dataset per LN is the safe
 * default). count <= 0 or a NULL references returns an empty list. Same
 * ownership contract as the chunk* functions above: caller owns the outer
 * list AND must LinkedList_destroyDeep(innerList, free) each inner list
 * before LinkedList_destroyStatic(outerList).
 */
LinkedList
MmsReportClientUseCases_groupReferencesByLn(const char* const* references, int count);

/*
 * Budget-aware middle ground between the two functions above: like
 * _groupReferencesByLn, never merges two different LNs' leaves into one
 * dataset (each resulting chunk is LN-homogeneous) - but like
 * _chunkReferencesAcrossWholeDevice, still bounds each chunk to
 * maxAttributes, sub-chunking any one LN's own leaf set into more than one
 * chunk if it doesn't fit in one. The chunk ORDER is breadth-first across
 * LNs, not grouped by LN: every LN contributes its own first chunk before any
 * LN contributes a second, round-robin, until every LN's own chunks are
 * exhausted or `maxClusters` chunks have been produced in total (`< 0` =
 * uncapped) - so when the caller can only afford N total datasets, this
 * maximizes how many DISTINCT LNs get at least one dataset of their own,
 * instead of `_chunkReferencesAcrossWholeDevice`'s own posture of minimizing
 * total dataset count by freely merging small LNs together (that function
 * remains the right choice when the caller's goal is coverage-at-minimum-
 * dataset-count rather than breadth-of-distinct-LNs-covered - see
 * mms_report_client_connection.c's buildWholeDeviceClusterPlan for which one
 * it actually uses and why).
 *
 * `references` spanning multiple LNs (e.g.
 * IedModel_getReportableAttributeReferencesForWholeDevice's own output),
 * relies on the same per-LN contiguous-run ordering guarantee
 * `_groupReferencesByLn` already depends on. `maxAttributes <= 0` or
 * `count <= 0` returns an empty list. Same ownership contract as the other
 * chunk/group functions above: caller owns the outer list AND must
 * LinkedList_destroyDeep(innerList, free) each inner list before
 * LinkedList_destroyStatic(outerList).
 */
LinkedList
MmsReportClientUseCases_buildBreadthFirstPerLnClusters(const char* const* references, int count,
        int maxAttributes, int maxClusters);

/*
 * Converts one ACSI dot/bracket-form dataset member reference
 * ("LD/LN.DO[.SDO...].DA[FC]", the exact shape IedConnection_getDataSetDirectory
 * returns) into this feature's own "$"-joined member-reference convention
 * ("LD/LN$FC$DO[$SDO...][$DA]" - matches IedModel_getDataSetMemberReferences's
 * own output). The mirror image of MmsReportClientUseCases_buildWireMemberReferences
 * (which goes the OPPOSITE direction, "$"-joined -> dot/bracket form, for
 * IedConnection_createDataSet's input) but NOT the same function as
 * ied_model_online_loader's own IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef,
 * which strips the LD prefix (that feature's DataSetEntry_create needs
 * LD-free names - a different convention this feature's memberReferences[]
 * does not share). Used by mms_report_client_connection.c to turn a pulled
 * live dataset's own IedConnection_getDataSetDirectory member list into this
 * feature's standard reference form.
 *
 * Returns NULL (malformed input - no trailing "[FC]", no "." after the
 * "LD/LN" prefix, no "/" within that prefix, or allocation failure) rather
 * than a best-effort partial string; caller must free a non-NULL result and
 * should skip NULL ones. Any "(arrayIndex)" annotation on a path segment is
 * stripped, not preserved (this codebase does not model array indices
 * anywhere else).
 */
char*
MmsReportClientUseCases_convertAcsiRefToMemberReference(const char* acsiRef);

/*
 * Builds one MmsReportClientMemberRefCacheEntry* from an already-resolved
 * ordered member-reference array ("$"-joined, LD-prefixed "LD/LN$FC$DO[$DA]"
 * wire form - this feature's own convention) plus the dataset identity that
 * array came from (resolvedDatasetReference - see
 * MmsReportClientMemberRefCacheEntry's own doc comment in
 * mms_report_client_types.h for how this is later used to detect a
 * reconnect-time shape change). Callable both at MmsReportClient_start
 * (mms_report_client_api.c's buildMemberRefCache - tier 1's static SCL
 * dataset, and tier 3's provisional LN-fallback list) and post-connect from
 * mms_report_client_connection.c (tier 2's pulled live dataset, and tier 3's
 * actually-realized "@dyn_<ln>" list).
 *
 * Every member's Gap-4 decomposition/wire-type/semantics is resolved via the
 * ied_model *ForMemberReference accessors, keyed directly by each member's
 * own reference string rather than a (datasetReference, index) pair into a
 * registered DataSet - this works identically regardless of whether array's
 * members came from a real registered SCL DataSet, a live dataset resolved
 * over the wire with no local DataSet object at all, or a purely-local LN
 * leaf walk.
 *
 * `array`/`count` is consumed: ownership of array and every element
 * transfers in on success (the new entry's own memberReferences field), and
 * every element plus the array itself is freed on any failure path -
 * `array`/`count` must never be used again by the caller after this call
 * either way. Returns NULL (array/its contents still freed) on a NULL/empty
 * array or allocation failure. `resolvedDatasetReference` (nullable) is
 * duplicated into the new entry's own field of the same name.
 */
MmsReportClientMemberRefCacheEntry*
MmsReportClientUseCases_buildMemberRefCacheEntry(IedModelHandle iedModel, const char* rcbReference,
        char** array, int count, const char* resolvedDatasetReference);

/*
 * Pure doubling-with-cap backoff calculation. currentDelayMs == 0 means "no
 * prior failure yet" and returns initialMs; otherwise doubles currentDelayMs,
 * capped at maxMs. No threads/sockets touched here.
 */
uint32_t
MmsReportClientUseCases_computeNextBackoffDelay(uint32_t currentDelayMs, uint32_t initialMs, uint32_t maxMs);

#endif /* MMS_REPORT_CLIENT_USECASES_H_ */
