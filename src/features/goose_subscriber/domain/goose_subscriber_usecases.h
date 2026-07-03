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
 */
GooseSubscriberRecord*
GooseSubscriberUseCases_buildRecord(
        const char* goCbRef, const char* goId, const char* dataSet,
        uint32_t stNum, uint32_t sqNum, uint32_t confRev,
        bool test, bool needsCommission,
        uint32_t timeAllowedToLiveMs, uint64_t timestampMs,
        bool hasVlan, uint16_t vlanId, uint8_t vlanPrio, int32_t appId,
        const uint8_t srcMac[6], const uint8_t dstMac[6],
        const MmsValue* dataSetValues, int entryCount);

/* Frees a record built by GooseSubscriberUseCases_buildRecord, including every
 * entry's cloned value and the entries array. NULL-safe. */
void
GooseSubscriberUseCases_freeRecord(GooseSubscriberRecord* record);

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

#endif /* GOOSE_SUBSCRIBER_USECASES_H_ */
