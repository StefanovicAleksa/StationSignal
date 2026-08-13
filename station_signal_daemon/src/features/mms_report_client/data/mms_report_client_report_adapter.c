#define SS_LOG_FEATURE "mms_report_client"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_report_client/data/mms_report_client_report_adapter.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "log.h"

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

    if (!handle || !handle->reportCallback || !report) {
        /* Should be unreachable in practice (orchestration always wires the
         * callback before a report can arrive) - logged anyway, since a
         * report silently vanishing here with no trace would otherwise look
         * identical to "nothing happened" to anyone watching the daemon's
         * output, all the way up to a real device operator making a real
         * change and seeing no report anywhere. */
        SS_LOG_WARN("[mms_report_client] report arrived but cannot be delivered (handle=%p, "
                "reportCallback=%p, report=%p)\n", (void*) handle,
                handle ? (void*) handle->reportCallback : NULL, (void*) report);
        return;
    }

    char* rcbReference = ClientReport_getRcbReference(report);
    char* rptId = ClientReport_getRptId(report);
    bool buffered = lookupBuffered(handle, rcbReference);
    MmsReportClientMemberRefCacheEntry* fallback = lookupMemberRefCache(handle, rcbReference);

    MmsValue* dataSetValues = ClientReport_getDataSetValues(report);
    int entryCount = dataSetValues ? MmsValue_getArraySize(dataSetValues) : 0;

    ReasonForInclusion* reasons = NULL;
    const char** dataReferences = NULL;
    bool hasDataReference = ClientReport_hasDataReference(report);

    /* The one place that proves a report physically arrived at all - every
     * other fprintf in this feature is about enabling/resolving a dataset
     * BEFORE any report exists. Without this, a report that arrives and then
     * gets filtered out below (by design or by bug) is completely
     * indistinguishable, from the terminal, from the device never having
     * sent one in the first place. */
    SS_LOG_DEBUG("[mms_report_client] report received for '%s' rptId='%s' buffered=%d entries=%d "
            "hasDataReference=%d\n", rcbReference ? rcbReference : "?", rptId ? rptId : "?", buffered,
            entryCount, hasDataReference);

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

    /* Guarded by memberRefCacheLock - buildReportRecord reads/mutates
     * fallback->lastForwardedValues (the value-diff cache), and the
     * lastEntryId update just below mutates fallback->lastEntryId - both are
     * also read on the supervisor thread (enableOneTarget, on every
     * (re)enable), so this lock is the real cross-thread guard for both
     * fields now, not just cheap insurance. */
    Semaphore_wait(handle->memberRefCacheLock);

    /* Non-monotonic/duplicate EntryID guard - see
     * MmsReportClientUseCases_isEntryIdStale's own doc comment for the full
     * rationale (a real-hardware capture showing the same buffered entry
     * redelivered multiple times, interleaved with newer ones, within one
     * continuous session). A stale report is dropped entirely here, before
     * it ever reaches the value-diff cache/decomposition/cross-RCB dedup
     * below - forwarding it would let a genuinely-already-seen historical
     * value look like a real change relative to whatever was most recently
     * processed. fallback->lastEntryId is deliberately left untouched on
     * this path (a stale entry is by definition <= the current cached
     * value, so leaving it alone is correct either way). */
    if (fallback && MmsReportClientUseCases_isEntryIdStale(entryId, fallback->lastEntryId)) {
        SS_LOG_DEBUG("[mms_report_client] dropping stale/duplicate report for '%s' "
                "(entryId not newer than last cached)\n", rcbReference ? rcbReference : "?");
        Semaphore_post(handle->memberRefCacheLock);
        free(reasons);
        free(dataReferences);
        return;
    }

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            rcbReference, buffered, rptId,
            entryId != NULL, entryId,
            hasTimestamp, hasTimestamp ? ClientReport_getTimestamp(report) : 0,
            hasSeqNum, hasSeqNum ? ClientReport_getSeqNum(report) : 0,
            dataSetValues, reasons, dataReferences,
            fallback,
            entryCount);

    /* Track the most recent EntryID for this RCB unconditionally - whether
     * or not this report's entries survive the value-diff filter, we did
     * durably receive it, so a later reconnect must resume from here, not
     * re-request everything the server still has buffered. See
     * MmsReportClientMemberRefCacheEntry.lastEntryId's own doc comment. */
    if (fallback && entryId) {
        if (fallback->lastEntryId) MmsValue_delete(fallback->lastEntryId);
        fallback->lastEntryId = MmsValue_clone(entryId);
    }
    Semaphore_post(handle->memberRefCacheLock);

    free(reasons);
    free(dataReferences);

    /* Allocation failure building the record: nothing safe to deliver - drop
     * this report rather than risk the caller dereferencing a partial one. */
    if (!record) {
        SS_LOG_DEBUG("[mms_report_client] failed to build report record for '%s' - dropped\n",
                rcbReference ? rcbReference : "?");
        return;
    }

    /* record->entryCount > 0: survived the per-RCB hybrid event filter.
     * Cross-RCB duplicate-content suppression (a different, often
     * redundant/reserved RCB forwarding this exact same content) is no
     * longer this feature's concern - ipc_dispatcher's own dedup cache
     * (IpcDispatcherUseCases_shouldForwardWithinProtocol) catches it
     * downstream instead, at the one point both this feature's RCBs AND
     * goose_subscriber's GoCBs actually converge. */
    if (record->entryCount > 0) {
        SS_LOG_DEBUG("[mms_report_client] forwarding report for '%s' (%d entr%s) to ipc_dispatcher\n",
                rcbReference ? rcbReference : "?", record->entryCount, record->entryCount == 1 ? "y" : "ies");
        handle->reportCallback(handle->reportCallbackParam, record);
    } else {
        /* Every entry was filtered by the per-RCB hybrid event filter (a
         * periodic/no-reason entry whose value matched the last one forwarded
         * for THIS RCB) - nothing worth forwarding to ipc_dispatcher. The
         * callback (which would otherwise own destroying this record) never
         * runs, so free it here instead. */
        SS_LOG_DEBUG("[mms_report_client] report for '%s' had %d raw entr%s, 0 survived the "
                "per-RCB value-diff filter (bootstrap-seed or unchanged) - nothing forwarded\n",
                rcbReference ? rcbReference : "?", entryCount, entryCount == 1 ? "y" : "ies");
        MmsReportClientUseCases_freeReportRecord(record);
    }
}
