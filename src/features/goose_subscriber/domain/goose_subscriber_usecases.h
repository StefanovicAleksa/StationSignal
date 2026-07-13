#ifndef GOOSE_SUBSCRIBER_USECASES_H_
#define GOOSE_SUBSCRIBER_USECASES_H_

#include <stdint.h>
#include "features/goose_subscriber/domain/goose_subscriber_types.h"

/*
 * Pure logic - no GooseSubscriber/GooseReceiver awareness at all here, that's
 * entirely the data layer's (goose_subscriber_frame_adapter /
 * goose_subscriber_connection) job. Takes plain arguments (strings, MmsValue*
 * arrays) rather than the opaque GooseSubscriber type specifically so it stays
 * unit-testable: GooseSubscriber has no public constructor, but MmsValue does.
 */

/*
 * Builds a fully-owned, deep-copied GooseSubscriberRecord* from already-
 * extracted GOOSE message fields. dataSetValues (if non-NULL) must be a
 * MMS_ARRAY MmsValue with at least entryCount elements. Every value/string is
 * cloned/duplicated - none of the inputs are retained by reference. Returns
 * NULL on allocation failure.
 *
 * memberRefCache (nullable) supplies, per raw dataset position: the locally-
 * resolved reference (GOOSE always uses this - unlike MMS, there is no
 * server-supplied alternative), the Gap 4 structure-decomposition metadata
 * (memberLeafReferences[i]/memberLeafCounts[i] - a DO-level FCDA at position
 * i has its structured value flattened via GooseSubscriberUtils_flattenStructure
 * and expanded into memberLeafCounts[i] leaf entries instead of one, unless
 * the flattened leaf count doesn't actually match, in which case it falls
 * back to a single non-decomposed entry for that position - see buildEntries's
 * own comment for the full algorithm), and the value-diff cache
 * (leafSlotOffsets/lastForwardedValues) used by the group-aware event filter.
 * record->entryCount reflects Gap-4 expansion AND filtering, not the raw
 * entryCount param. Passing NULL disables Gap 4 decomposition, reference
 * resolution, AND the value-diff filter for this call (every entry is a
 * plain 1:1 passthrough with a NULL reference, always forwarded) - matches
 * mms_report_client's own NULL-passthrough convention.
 *
 * Unlike mms_report_client, GOOSE has no ReasonForInclusion-equivalent
 * signal at all, so there is no "trust the server unconditionally" branch -
 * every candidate is diff-gated against the cache; a NULL cache slot
 * (nothing forwarded yet for that position - true on the very first frame
 * ever for a target, and again after any STALE/INVALID_STATE -> VALID
 * recovery, see GooseSubscriberUseCases_resetValueDiffCache) is the only
 * thing that unconditionally survives, which is GOOSE's equivalent of an
 * MMS GI snapshot. A value/quality (or other sibling DA) pair still travels
 * together via the same group-anchor "drag-along" rule mms_report_client
 * uses: if any candidate resolving to the same "$"-prefix anchor qualifies,
 * every candidate in that group forwards.
 */
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
        int entryCount);

/* Frees a record built by GooseSubscriberUseCases_buildRecord, including every
 * entry's cloned value and the entries array. NULL-safe. */
void
GooseSubscriberUseCases_freeRecord(GooseSubscriberRecord* record);

/*
 * Value-diff check for the group-aware filter's diff-gate. cached is the
 * last value actually cached for this exact wire position (NULL means
 * nothing has ever been cached yet). Returns true (duplicate, should be
 * dropped) only when cached is non-NULL AND bit-for-bit equal
 * (MmsValue_equals) to newValue - identical semantics to
 * MmsReportClientUseCases_isDuplicateValue. A NULL cached value always
 * returns false (not a duplicate), but the caller (shouldForwardAndUpdateCache,
 * goose_subscriber_usecases.c) treats a NULL cache as a bootstrap event and
 * seeds it WITHOUT forwarding - so the first frame ever for a target, or the
 * first frame after a recovery, never itself reaches the caller's record
 * callback, only the first genuine change afterward does. Exposed for direct
 * unit testing, mirroring that function's own precedent.
 */
bool
GooseSubscriberUseCases_isDuplicateValue(const MmsValue* cached, const MmsValue* newValue);

/*
 * Resets one target's value-diff cache in place: frees every populated slot
 * of memberRefCache->lastForwardedValues (MmsValue_delete) and sets it back
 * to NULL, mirroring MmsReportClientUseCases_resetValueDiffCache. Called by
 * the frame adapter on every STALE/INVALID_STATE -> VALID transition,
 * alongside the existing hasForwardedStNum reset, so the first frame after a
 * recovery is treated as a fresh bootstrap event (cache-seed only, never
 * forwarded) rather than being diff-filtered against stale pre-outage
 * values. NULL-safe.
 */
void
GooseSubscriberUseCases_resetValueDiffCache(GooseSubscriberMemberRefCache* memberRefCache);

/*
 * Pure edge-detection: given the previous and current GooseSubscriber_isValid()
 * results, decides whether a status transition occurred. Returns true and
 * fills *outStatus only on a transition (wasValid != isValid) - a caller must
 * not fire a status callback on a no-op poll. When isValid transitions to
 * false, the caller is responsible for choosing STALE vs INVALID_STATE based
 * on GooseSubscriber_getParseError() - that decision needs the live
 * GooseSubscriber handle, so it isn't made here.
 */
bool
GooseSubscriberUseCases_detectStatusTransition(bool wasValid, bool isValid, GooseSubscriberStatus* outStatus);

/*
 * Pure interval computation for the liveness timer. Returns configuredMs
 * verbatim if >0 (explicit caller override always wins). Otherwise derives
 * from the shortest currently-observed TimeAllowedToLive across all targets
 * (minTalMs), floored at 50ms so the poll never busy-loops. minTalMs <= 0
 * means "no TAL observed yet for any target" (TAL is only known from a
 * received GOOSE message, not from SCL) - falls back to a fixed 1000ms
 * default in that case.
 */
uint32_t
GooseSubscriberUseCases_computeLivenessPollIntervalMs(uint32_t configuredMs, int32_t minTalMs);

/*
 * Cross-target duplicate-content suppression - see
 * GooseSubscriberCrossTargetDedupCache's own doc comment for the full
 * rationale (independent GoCBs publishing the same underlying event).
 * Compares (goCbRef, entries[0..entryCount)) against cache's own
 * last-forwarded content: if cache has prior content, goCbRef differs from
 * the one that produced it, AND every (reference, value) pair matches
 * positionally, this is a duplicate - returns false (do not forward),
 * leaving cache untouched. Otherwise (nothing cached yet, same goCbRef as
 * before, or genuinely different content) returns true and replaces cache's
 * content with this call's (goCbRef, entries) - so a suppressed duplicate
 * never disturbs the established baseline, but every other case refreshes
 * it. NULL-safe on cache (returns true, i.e. always forward, if cache is
 * NULL).
 */
bool
GooseSubscriberUseCases_shouldForwardAcrossTarget(GooseSubscriberCrossTargetDedupCache* cache,
        const char* goCbRef, const GooseSubscriberEntry* entries, int entryCount);

/* Frees cache's owned goCbRef/entries and resets it back to empty ("nothing
 * forwarded yet"). NULL-safe. Used both internally by
 * GooseSubscriberUseCases_shouldForwardAcrossTarget (to replace stale
 * content) and by GooseSubscription_destroy (final cleanup). */
void
GooseSubscriberUseCases_destroyCrossTargetDedupCache(GooseSubscriberCrossTargetDedupCache* cache);

/*
 * Pure GOOSE-heartbeat dedup check. A publisher retransmits at every
 * MinTime/MaxTime interval regardless of whether the data changed (sqNum
 * increments on every retransmit; stNum only increments on an actual
 * change). Returns true iff newStNum duplicates the last stNum this feature
 * actually forwarded for this target - i.e. this is a heartbeat
 * retransmission, nothing new to deliver. hasForwardedStNum=false always
 * returns false (never a duplicate - nothing forwarded yet).
 */
bool
GooseSubscriberUseCases_isDuplicateStNum(bool hasForwardedStNum, uint32_t lastForwardedStNum, uint32_t newStNum);

#endif /* GOOSE_SUBSCRIBER_USECASES_H_ */
