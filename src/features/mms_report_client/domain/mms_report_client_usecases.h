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

/* LinkedListValueDeleteFunction-compatible: frees an
 * MmsReportClientMemberRefCacheEntry, including the Gap 4 (memberLeafReferences)
 * cache and the value-diff cache (leafSlotOffsets/lastForwardedValues)
 * alongside the original rcbReference/memberReferences. NULL-safe. */
void
MmsReportClientUseCases_destroyMemberRefCacheEntry(void* entry);

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
