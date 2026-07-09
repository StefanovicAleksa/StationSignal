#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_report_client/data/mms_report_client_report_adapter.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"

/* ClientReport doesn't expose buffered-ness directly - look it up in our own
 * cached target list (keyed by rcbReference) instead of re-deriving it. */
static bool
lookupBuffered(MmsReportClientHandle handle, const char* rcbReference) {
    if (!handle->targets || !rcbReference) return false;

    LinkedList element = LinkedList_getNext(handle->targets);
    while (element) {
        ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);
        if (target->objectReference && strcmp(target->objectReference, rcbReference) == 0) {
            return target->buffered;
        }
        element = LinkedList_getNext(element);
    }
    return false;
}

/* Locally-resolved fallback for entries whose server-side report omitted a
 * data-reference, plus the Gap 4 decomposition metadata and the value-diff
 * cache used by the hybrid event filter - see MmsReportClientMemberRefCacheEntry's
 * doc comment. Returns a non-const pointer because buildReportRecord mutates
 * lastForwardedValues in place as reports are processed. */
static MmsReportClientMemberRefCacheEntry*
lookupMemberRefCache(MmsReportClientHandle handle, const char* rcbReference) {
    if (!handle->memberRefCache || !rcbReference) return NULL;

    LinkedList element = LinkedList_getNext(handle->memberRefCache);
    while (element) {
        MmsReportClientMemberRefCacheEntry* entry =
                (MmsReportClientMemberRefCacheEntry*) LinkedList_getData(element);
        if (entry->rcbReference && strcmp(entry->rcbReference, rcbReference) == 0) return entry;
        element = LinkedList_getNext(element);
    }
    return NULL;
}

void
MmsReportClientReportAdapter_onReport(void* parameter, ClientReport report) {
    MmsReportClientHandle handle = (MmsReportClientHandle) parameter;

    /* Debug trace: confirms whether the server-pushed report is actually
     * arriving at this callback at all, before anything downstream
     * (record building, the reportCallback -> ipc_dispatcher -> websocket
     * chain) has a chance to swallow or drop it silently. */
    fprintf(stderr, "[mms_report_client] onReport fired: handle=%p reportCallback=%p report=%p\n",
            (void*) handle, handle ? (void*) handle->reportCallback : NULL, (void*) report);

    if (!handle || !handle->reportCallback || !report) return;

    char* rcbReference = ClientReport_getRcbReference(report);
    char* rptId = ClientReport_getRptId(report);
    bool buffered = lookupBuffered(handle, rcbReference);
    MmsReportClientMemberRefCacheEntry* fallback = lookupMemberRefCache(handle, rcbReference);

    MmsValue* dataSetValues = ClientReport_getDataSetValues(report);
    int entryCount = dataSetValues ? MmsValue_getArraySize(dataSetValues) : 0;

    fprintf(stderr, "[mms_report_client] onReport: rcbReference=%s entryCount=%d\n",
            rcbReference ? rcbReference : "(null)", entryCount);

    ReasonForInclusion* reasons = NULL;
    const char** dataReferences = NULL;
    bool hasDataReference = ClientReport_hasDataReference(report);

    if (entryCount > 0) {
        reasons = malloc(sizeof(ReasonForInclusion) * (size_t) entryCount);
        if (hasDataReference) dataReferences = malloc(sizeof(char*) * (size_t) entryCount);

        for (int i = 0; i < entryCount; i++) {
            if (reasons) reasons[i] = ClientReport_getReasonForInclusion(report, i);
            if (dataReferences) dataReferences[i] = ClientReport_getDataReference(report, i);
        }
    }

    MmsValue* entryId = ClientReport_getEntryId(report);
    bool hasTimestamp = ClientReport_hasTimestamp(report);
    bool hasSeqNum = ClientReport_hasSeqNum(report);

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            rcbReference, buffered, rptId,
            entryId != NULL, entryId,
            hasTimestamp, hasTimestamp ? ClientReport_getTimestamp(report) : 0,
            hasSeqNum, hasSeqNum ? ClientReport_getSeqNum(report) : 0,
            dataSetValues, reasons, dataReferences,
            fallback,
            entryCount);

    free(reasons);
    free(dataReferences);

    /* Allocation failure building the record: nothing safe to deliver - drop
     * this report rather than risk the caller dereferencing a partial one. */
    if (!record) return;

    /* record->entryCount > 0: survived the per-RCB hybrid event filter.
     * shouldForwardAcrossRcb is the second, independent gate: even a report
     * that's genuinely new/changed AS FAR AS THIS RCB IS CONCERNED can still
     * be an exact duplicate of what a DIFFERENT (often redundant/reserved)
     * RCB on this same client just forwarded a moment earlier - see
     * MmsReportClientCrossRcbDedupCache's own doc comment. */
    if (record->entryCount > 0 && MmsReportClientUseCases_shouldForwardAcrossRcb(
            &handle->crossRcbDedupCache, record->rcbReference, record->entries, record->entryCount)) {
        handle->reportCallback(handle->reportCallbackParam, record);
    } else {
        /* Either every entry was filtered by the per-RCB hybrid event filter
         * (a periodic/no-reason entry whose value matched the last one
         * forwarded for THIS RCB), or this exact content was just forwarded
         * a moment ago from a different RCB - nothing worth forwarding to
         * ipc_dispatcher either way. The callback (which would otherwise own
         * destroying this record) never runs, so free it here instead. */
        MmsReportClientUseCases_freeReportRecord(record);
    }
}
