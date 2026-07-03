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

static MmsReportEntry*
buildEntries(const MmsValue* dataSetValues, const ReasonForInclusion* reasons,
        const char* const* dataReferences,
        const char* const* fallbackReferences, int fallbackCount,
        int entryCount) {
    if (entryCount <= 0) return NULL;

    MmsReportEntry* entries = calloc((size_t) entryCount, sizeof(MmsReportEntry));
    if (!entries) return NULL;

    for (int i = 0; i < entryCount; i++) {
        if (dataSetValues) {
            MmsValue* element = MmsValue_getElement((MmsValue*) dataSetValues, i);
            entries[i].value = element ? MmsValue_clone(element) : NULL;
        }
        const char* ref = (dataReferences && dataReferences[i]) ? dataReferences[i]
                : (fallbackReferences && i < fallbackCount) ? fallbackReferences[i] : NULL;
        if (ref) entries[i].reference = MmsReportClientUtils_safeStringDup(ref);
        entries[i].reason = reasons ? reasons[i] : IEC61850_REASON_UNKNOWN;
    }

    return entries;
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
        const char* const* fallbackReferences, int fallbackCount,
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

    record->entries = buildEntries(dataSetValues, reasons, dataReferences, fallbackReferences, fallbackCount,
            entryCount);
    record->entryCount = record->entries ? entryCount : 0;

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
    free(e->rcbReference);
    free(e);
}
