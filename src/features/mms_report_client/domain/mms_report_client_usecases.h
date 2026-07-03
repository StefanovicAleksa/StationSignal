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
        const char* const* fallbackReferences, int fallbackCount,
        int entryCount);

/* Frees a record built by MmsReportClientUseCases_buildReportRecord, including
 * every entry's cloned value/reference and the entries array. NULL-safe. */
void
MmsReportClientUseCases_freeReportRecord(MmsReportRecord* record);

/* LinkedListValueDeleteFunction-compatible: frees an
 * MmsReportClientMemberRefCacheEntry (rcbReference, each memberReferences[i],
 * the array, then the struct itself). NULL-safe. */
void
MmsReportClientUseCases_destroyMemberRefCacheEntry(void* entry);

/*
 * Pure doubling-with-cap backoff calculation. currentDelayMs == 0 means "no
 * prior failure yet" and returns initialMs; otherwise doubles currentDelayMs,
 * capped at maxMs. No threads/sockets touched here.
 */
uint32_t
MmsReportClientUseCases_computeNextBackoffDelay(uint32_t currentDelayMs, uint32_t initialMs, uint32_t maxMs);

#endif /* MMS_REPORT_CLIENT_USECASES_H_ */
