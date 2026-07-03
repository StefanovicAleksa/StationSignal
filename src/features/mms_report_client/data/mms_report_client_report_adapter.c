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

void
MmsReportClientReportAdapter_onReport(void* parameter, ClientReport report) {
    MmsReportClientHandle handle = (MmsReportClientHandle) parameter;
    if (!handle || !handle->reportCallback || !report) return;

    char* rcbReference = ClientReport_getRcbReference(report);
    char* rptId = ClientReport_getRptId(report);
    bool buffered = lookupBuffered(handle, rcbReference);

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

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            rcbReference, buffered, rptId,
            entryId != NULL, entryId,
            hasTimestamp, hasTimestamp ? ClientReport_getTimestamp(report) : 0,
            hasSeqNum, hasSeqNum ? ClientReport_getSeqNum(report) : 0,
            dataSetValues, reasons, dataReferences, entryCount);

    free(reasons);
    free(dataReferences);

    /* Allocation failure building the record: nothing safe to deliver - drop
     * this report rather than risk the caller dereferencing a partial one. */
    if (record) handle->reportCallback(handle->reportCallbackParam, record);
}
