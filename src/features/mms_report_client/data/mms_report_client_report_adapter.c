#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal_time.h"
#include "features/mms_report_client/data/mms_report_client_report_adapter.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"

/* Temporary diagnostic aid (see CLAUDE.md's ied_model_online_loader bullet /
 * the plan this landed under) - real-hardware reports of "<unsupported:structure>"
 * on every MMS data point persisted even after the LD-prefix fix, so these log
 * files give direct, persistent visibility into what the wire actually
 * delivers vs. what survives Gap-4 decomposition/filtering. Opens/closes the
 * file per call rather than holding a long-lived FILE* - keeps every write
 * flushed immediately and needs no added locking, since each fprintf here is
 * well under PIPE_BUF and fopen(..., "a") is append-only, so concurrent
 * report-reader threads from different devices (device_manager can run
 * several at once) interleave safely at the line level. */
static void
appendDebugLog(const char* path, const char* text) {
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[%llu] %s\n", (unsigned long long) Hal_getTimeInMs(), text);
    fclose(f);
}

/* TEMPORARY diagnostic aid - see mms_report_client_connection.c's own
 * identical-purpose log (SEND side of this same investigation). Logs what
 * the server actually sends back per buffered report - seqNum and EntryID -
 * for direct correlation: if these never advance past what enableOneTarget
 * requested to resume from across a burst of reports that look like a
 * redelivered backlog, that confirms the server (or our own unconditional GI
 * request) isn't honoring EntryID resumption. Remove once root-caused. */
#define MMS_REPORT_CLIENT_ENTRY_ID_DEBUG_LOG_PATH "ied_reporter_debug_entryid.log"

static void
hexDump(const uint8_t* bytes, int size, char* out, size_t outSize) {
    size_t pos = 0;
    for (int i = 0; i < size && pos + 3 < outSize; i++) {
        pos += (size_t) snprintf(out + pos, outSize - pos, "%02X", bytes[i]);
    }
    out[pos] = '\0';
}

/* Renders one MmsValue into a fixed local buffer via MmsValue_printToBuffer
 * (third_party/include/mms_value.h - "for debugging purposes only", but
 * exactly what's needed here since it can render an untouched MMS_STRUCTURE
 * too, not just scalars). NULL-safe. */
static void
debugFormatValue(const MmsValue* value, char* buffer, size_t bufferSize) {
    if (!value) {
        snprintf(buffer, bufferSize, "<null>");
        return;
    }
    MmsValue_printToBuffer((MmsValue*) value, buffer, (int) bufferSize);
}

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

    if (!handle || !handle->reportCallback || !report) return;

    char* rcbReference = ClientReport_getRcbReference(report);
    char* rptId = ClientReport_getRptId(report);
    bool buffered = lookupBuffered(handle, rcbReference);
    MmsReportClientMemberRefCacheEntry* fallback = lookupMemberRefCache(handle, rcbReference);

    MmsValue* dataSetValues = ClientReport_getDataSetValues(report);
    int entryCount = dataSetValues ? MmsValue_getArraySize(dataSetValues) : 0;

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

    if (buffered) {
        char line[384];
        if (entryId) {
            char hex[256];
            hexDump(MmsValue_getOctetStringBuffer(entryId), MmsValue_getOctetStringSize(entryId),
                    hex, sizeof(hex));
            snprintf(line, sizeof(line), "RECV rcb=%s entryId=%s hasSeqNum=%d seqNum=%u entryCount=%d",
                    rcbReference ? rcbReference : "(null)", hex, hasSeqNum,
                    hasSeqNum ? ClientReport_getSeqNum(report) : 0, entryCount);
        } else {
            snprintf(line, sizeof(line), "RECV rcb=%s entryId=(none) hasSeqNum=%d seqNum=%u entryCount=%d",
                    rcbReference ? rcbReference : "(null)", hasSeqNum,
                    hasSeqNum ? ClientReport_getSeqNum(report) : 0, entryCount);
        }
        appendDebugLog(MMS_REPORT_CLIENT_ENTRY_ID_DEBUG_LOG_PATH, line);
    }

    /* Debug log point 1: raw report exactly as libiec61850 delivered it,
     * before Gap-4 decomposition/value-diff filtering touch it at all. */
    {
        char header[256];
        snprintf(header, sizeof(header), "==== rcbReference=%s entryCount=%d ====",
                rcbReference ? rcbReference : "(null)", entryCount);
        appendDebugLog("/tmp/ied_reporter_debug_mms_before.log", header);

        for (int i = 0; i < entryCount; i++) {
            MmsValue* rawValue = dataSetValues ? MmsValue_getElement(dataSetValues, i) : NULL;
            char valueBuf[512];
            debugFormatValue(rawValue, valueBuf, sizeof(valueBuf));

            char line[896];
            snprintf(line, sizeof(line), "  [%d] type=%s value=%s reason=%s dataReference=%s",
                    i,
                    rawValue ? MmsValue_getTypeString(rawValue) : "(none)",
                    valueBuf,
                    reasons ? ReasonForInclusion_getValueAsString(reasons[i]) : "(unknown)",
                    (dataReferences && dataReferences[i]) ? dataReferences[i] : "(none)");
            appendDebugLog("/tmp/ied_reporter_debug_mms_before.log", line);
        }
    }

    /* Guarded by memberRefCacheLock - buildReportRecord reads/mutates
     * fallback->lastForwardedValues (the value-diff cache), and the
     * lastEntryId update just below mutates fallback->lastEntryId - both are
     * also read on the supervisor thread (enableOneTarget, on every
     * (re)enable), so this lock is the real cross-thread guard for both
     * fields now, not just cheap insurance. */
    Semaphore_wait(handle->memberRefCacheLock);
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
    if (!record) return;

    /* Debug log point 2: the decomposed/filtered record, about to be
     * forwarded (pending only the cross-RCB dedup check below) - the direct
     * view of whether Gap-4 decomposition actually ran for this device. */
    {
        char header[256];
        snprintf(header, sizeof(header), "==== rcbReference=%s entryCount=%d ====",
                record->rcbReference ? record->rcbReference : "(null)", record->entryCount);
        appendDebugLog("/tmp/ied_reporter_debug_mms_after.log", header);

        for (int i = 0; i < record->entryCount; i++) {
            MmsReportEntry* entry = &record->entries[i];
            char valueBuf[512];
            char previousValueBuf[512];
            debugFormatValue(entry->value, valueBuf, sizeof(valueBuf));
            debugFormatValue(entry->previousValue, previousValueBuf, sizeof(previousValueBuf));

            char line[1152];
            snprintf(line, sizeof(line), "  [%d] reference=%s type=%s value=%s previousValue=%s",
                    i,
                    entry->reference ? entry->reference : "(null)",
                    entry->value ? MmsValue_getTypeString(entry->value) : "(none)",
                    valueBuf,
                    previousValueBuf);
            appendDebugLog("/tmp/ied_reporter_debug_mms_after.log", line);
        }
    }

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
