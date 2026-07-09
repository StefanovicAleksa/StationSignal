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
 * reason is a genuine bitmask - ClientReport_getReasonForInclusion can OR
 * multiple bits together (e.g. DATA_CHANGE|INTEGRITY if a value happens to
 * change exactly when an integrity scan fires), so this must never be an
 * equality check. Returns true iff reason carries any real-change bit
 * (DATA_CHANGE/QUALITY_CHANGE/DATA_UPDATE), regardless of any periodic bit
 * (INTEGRITY/GI) also present - the server is vouching for a genuine change,
 * so buildEntries trusts it unconditionally rather than running the
 * value-diff check. False for INTEGRITY/GI-only entries and for
 * UNKNOWN/NOT_INCLUDED (servers that omit reason-for-inclusion entirely) -
 * both fall through to the value-diff cache instead of being trusted or
 * dropped outright.
 */
bool
MmsReportClientUseCases_hasRealChangeReason(ReasonForInclusion reason);

/*
 * Value-diff check for the hybrid filter's non-real-change branch. cached is
 * the last value actually forwarded for this exact wire position (NULL means
 * nothing has ever been forwarded yet). Returns true (duplicate, should be
 * dropped) only when cached is non-NULL AND bit-for-bit equal
 * (MmsValue_equals) to newValue - a NULL cached value always returns false,
 * which is what lets the first-ever report for a position (typically the
 * startup GI snapshot) survive and seed the cache.
 */
bool
MmsReportClientUseCases_isDuplicateValue(const MmsValue* cached, const MmsValue* newValue);

/* LinkedListValueDeleteFunction-compatible: frees an
 * MmsReportClientMemberRefCacheEntry, including the Gap 4 (memberLeafReferences)
 * cache and the value-diff cache (leafSlotOffsets/lastForwardedValues)
 * alongside the original rcbReference/memberReferences. NULL-safe. */
void
MmsReportClientUseCases_destroyMemberRefCacheEntry(void* entry);

/*
 * Resets one RCB's value-diff cache in place: frees every populated slot of
 * entry->lastForwardedValues (MmsValue_delete) and sets it back to NULL,
 * mirroring the free loop inside MmsReportClientUseCases_destroyMemberRefCacheEntry
 * but WITHOUT freeing the lastForwardedValues array itself or any other field -
 * this entry keeps living, only its cached values are forgotten. Called by
 * enableOneTarget every time it successfully (re-)enables an RCB's RptEna,
 * both on the very first connect and on every reconnect (idempotent either
 * way - a freshly built cache is already all-NULL). Without this, a
 * reconnect's GI-triggered snapshot report gets diff-checked against the
 * stale pre-disconnect cache and is wrongly dropped as an unchanged
 * duplicate. NULL-safe/no-op if entry or entry->lastForwardedValues is NULL.
 */
void
MmsReportClientUseCases_resetValueDiffCache(MmsReportClientMemberRefCacheEntry* entry);

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
 * Pure doubling-with-cap backoff calculation. currentDelayMs == 0 means "no
 * prior failure yet" and returns initialMs; otherwise doubles currentDelayMs,
 * capped at maxMs. No threads/sockets touched here.
 */
uint32_t
MmsReportClientUseCases_computeNextBackoffDelay(uint32_t currentDelayMs, uint32_t initialMs, uint32_t maxMs);

#endif /* MMS_REPORT_CLIENT_USECASES_H_ */
