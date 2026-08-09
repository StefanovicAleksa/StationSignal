#ifndef MMS_DATASET_MANAGER_USECASES_H_
#define MMS_DATASET_MANAGER_USECASES_H_

#include "linked_list.h"
#include "stdbool_compat.h"

/*
 * Pure logic - no IedConnection/ClientReportControlBlock awareness at all
 * here, that's entirely the data layer's (mms_dataset_manager_discovery /
 * _provisioning / _naming) job. Takes plain arguments (reference strings,
 * counts, budgets) rather than any live handle specifically so it stays
 * unit-testable with no server round-trip: every function below is either
 * string manipulation or arithmetic, and none of them allocate anything the
 * caller doesn't own outright.
 *
 * Deliberately does not include this feature's own types header either -
 * nothing here needs MmsDatasetResolution or the handle. LinkedList is the
 * only third-party type, and only as a return container.
 */

/*
 * True once this connect cycle's own dataset-count budget (a copy of SCL's
 * <Services><DynDataSet max="N"/> or <ConfDataSet max="N"/>, see
 * IedModel_getDynDataSetMax/_getConfDataSetMax) has been fully spent on new
 * self-created datasets - see createAndCacheDynamicDatasetAttempt's own doc
 * comment (mms_dataset_manager_provisioning.c) for where this gates further
 * createDataSet attempts. remainingBudget == -1 (SCL never declared a cap)
 * must never be "exhausted" - only a genuine 0 counts. Extracted as a pure
 * predicate purely so it's directly unit-testable without a live server
 * round-trip.
 */
bool
MmsDatasetManagerUseCases_isDynamicDatasetBudgetExhausted(int remainingBudget);

/*
 * Corrects the naive "just copy SCL's own declared max" budget seeding by
 * accounting for datasets that already exist on the server at the start of
 * this connect cycle (discovered via IedConnection_getLogicalDeviceDataSets,
 * mms_dataset_manager_discovery.c's MmsDatasetManagerDiscovery_findExistingServerDatasets)
 * - a device's REAL remaining budget is smaller than its declared max whenever
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
MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(int sclMax, int existingDatasetCount);

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
MmsDatasetManagerUseCases_buildWireMemberReferences(const char* const* memberReferences, int count);

/*
 * Extracts the DO group key (the 3rd "$"-segment: "LD/LN$FC$DO[$SDO...]$DA" -
 * same format as MmsDatasetManagerUseCases_buildWireMemberReferences's own input)
 * from one member reference, for MmsDatasetManagerUseCases_chunkReferencesByDoGroup's
 * own DO-atomic grouping below. A malformed reference (fewer than 3
 * "$"-segments) returns a copy of the whole string as its own singleton
 * group, rather than erroring - this function only groups, it never rejects.
 * Caller owns the returned string (free).
 */
char*
MmsDatasetManagerUseCases_extractDoGroupKey(const char* memberReference);

/*
 * Greedily packs `references` (already scoped to one LN, in their existing
 * FC=ST-then-MX declaration order - see
 * IedModel_getReportableAttributeReferencesForLogicalNode) into DO-atomic
 * chunks of at most maxAttributes members each: one Data Object's own leaves
 * (stVal/q/t, etc.) are never split across two chunks, even if that DO alone
 * exceeds maxAttributes - that DO becomes its own oversized chunk instead,
 * handled downstream exactly like any other self-create failure, not
 * specially.
 *
 * Strategy is deliberately simple: greedy, order-preserving, DO-atomic - no
 * bin-packing optimization, no reordering to minimize chunk count. This is
 * also the one piece of logic likely reusable as-is by a future no-SCL
 * empirical-discovery follow-up (deferred - see GAP3_DYNAMIC_DATASET_NOTES.md),
 * once that path has discovered a working size limit some other way; it
 * doesn't care where maxAttributes came from.
 *
 * Also the split strategy _chunkReferencesAcrossWholeDevice below falls back
 * to for a single oversized LN's own leaves (a whole-device chunk may never
 * mix a different LN's leaves with a split LN's spillover - see that
 * function's own doc comment), on top of remaining the per-LN granularity a
 * deferred no-SCL empirical-discovery path would want back.
 *
 * maxAttributes <= 0 or count <= 0 returns an empty list (nothing to chunk).
 * Returns a LinkedList of LinkedList-of-owned-char*, one inner list per chunk
 * in chunk order. Caller owns the outer list AND must
 * LinkedList_destroyDeep(innerList, free) each inner list before
 * LinkedList_destroyStatic(outerList).
 */
LinkedList
MmsDatasetManagerUseCases_chunkReferencesByDoGroup(const char* const* references, int count, int maxAttributes);

/*
 * Whole-device counterpart of MmsDatasetManagerUseCases_chunkReferencesByDoGroup,
 * safe for `references` spanning MULTIPLE LNs (e.g.
 * IedModel_getReportableAttributeReferencesForWholeDevice's own output)
 * rather than one LN's own leaf list. LN-preserving: internally groups by LN
 * first (via groupReferencesByLn) and packs WHOLE LN groups into a chunk
 * while they fit under maxAttributes - a resulting chunk MAY legitimately
 * span several different (small) LNs' worth of leaves when EVERY contributing
 * LN fits entirely, maximizing device coverage within a tight total
 * dataset-count budget (this combining is the intended behavior, not a bug).
 * What it never does: mix a different LN's leaves into a chunk that already
 * holds a PARTIAL spillover from a different, oversized LN - a single LN
 * whose own leaf count exceeds maxAttributes is instead split DO-atomically
 * (via chunkReferencesByDoGroup, scoped to that LN alone) into one or more
 * chunks reserved for that LN only. Earlier versions of this function grouped
 * purely by "LD/LN$FC$DO" key and bin-packed consecutive DO-groups by fit
 * alone, with no LN-boundary awareness at all - a chunk (and therefore the
 * one RCB dataset it becomes) could freely mix the tail of one LN with the
 * head of a completely unrelated one, with no functional/semantic
 * relationship between the values it grouped together. That "no intent"
 * grouping is fixed here; see MmsDatasetManagerProvisioning_runClaimPass for
 * the separate (and more consequential) fix to the same-reference-in-two-
 * datasets duplication this whole-device pool could otherwise still produce.
 */
LinkedList
MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(const char* const* references, int count,
        int maxAttributes);

/*
 * String-set difference: returns the subset of `leaves` NOT present in
 * `claimedLeaves` (owned char* list), preserving `leaves`' own order. Used by
 * MmsDatasetManagerProvisioning_buildWholeDeviceClusterPlan to exclude
 * whatever MmsDatasetManagerProvisioning_runClaimPass's own tier 2/3 claim
 * pass already covered elsewhere this cycle, before chunking the remainder -
 * see that pass's own doc comment for why this exclusion matters. `leaves`
 * NULL/count<=0 or `claimedLeaves` NULL/empty returns a full copy (nothing to
 * exclude). Pure string comparison, no third-party/network calls - takes a
 * LinkedList for `claimedLeaves` (rather than a plain array like `leaves`)
 * purely because that's the natural shape the claim pass already builds it
 * in; still no library/data-layer dependency. Caller owns the result:
 * LinkedList_destroyDeep(result, free).
 */
LinkedList
MmsDatasetManagerUseCases_filterOutClaimedLeaves(const char* const* leaves, int count, LinkedList claimedLeaves);

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
MmsDatasetManagerUseCases_groupReferencesByLn(const char* const* references, int count);

/*
 * Converts one ACSI dot/bracket-form dataset member reference
 * ("LD/LN.DO[.SDO...].DA[FC]", the exact shape IedConnection_getDataSetDirectory
 * returns) into this codebase's "$"-joined member-reference convention
 * ("LD/LN$FC$DO[$SDO...][$DA]" - matches IedModel_getDataSetMemberReferences's
 * own output, and mms_report_client's own memberReferences[] convention). The
 * mirror image of MmsDatasetManagerUseCases_buildWireMemberReferences (which goes
 * the OPPOSITE direction, "$"-joined -> dot/bracket form, for
 * IedConnection_createDataSet's input) but NOT the same function as
 * ied_model_online_loader's own IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef,
 * which strips the LD prefix (that feature's DataSetEntry_create needs
 * LD-free names - a different convention this one does not share). Used by
 * mms_dataset_manager_discovery.c to turn a pulled live/adopted dataset's own
 * IedConnection_getDataSetDirectory member list into the standard reference
 * form its caller decodes reports against.
 *
 * Returns NULL (malformed input - no trailing "[FC]", no "." after the
 * "LD/LN" prefix, no "/" within that prefix, or allocation failure) rather
 * than a best-effort partial string; caller must free a non-NULL result and
 * should skip NULL ones. Any "(arrayIndex)" annotation on a path segment is
 * stripped, not preserved (this codebase does not model array indices
 * anywhere else).
 */
char*
MmsDatasetManagerUseCases_convertAcsiRefToMemberReference(const char* acsiRef);

#endif /* MMS_DATASET_MANAGER_USECASES_H_ */
