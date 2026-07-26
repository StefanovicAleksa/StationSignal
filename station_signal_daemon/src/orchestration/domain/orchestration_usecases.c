#include <stddef.h>
#include "orchestration/domain/orchestration_usecases.h"

SclBootstrapResult*
OrchestrationUseCases_selectAndDetachFirstRetrieved(LinkedList results) {
    if (!results) return NULL;

    LinkedList element = LinkedList_getNext(results);
    while (element) {
        SclBootstrapResult* result = (SclBootstrapResult*) LinkedList_getData(element);

        if (result && result->status == SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED) {
            LinkedList_remove(results, result);
            return result;
        }

        element = LinkedList_getNext(element);
    }

    return NULL;
}

SclBootstrapCandidateStatus
OrchestrationUseCases_summarizeBootstrapFailure(LinkedList results) {
    LinkedList last = results ? LinkedList_getLastElement(results) : NULL;
    if (!last) return SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER;

    SclBootstrapResult* result = (SclBootstrapResult*) LinkedList_getData(last);
    if (!result) return SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER;

    return result->status;
}
