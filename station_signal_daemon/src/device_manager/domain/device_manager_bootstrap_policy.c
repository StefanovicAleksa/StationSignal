#include "device_manager/domain/device_manager_bootstrap_policy.h"

OrchestrationError
DeviceManagerBootstrapPolicy_run(OrchestrationHandle handle, const char* host, int mmsPort,
        const char* iedName, const char* interfaceId, const char* sclFilePath,
        const char* acseAuthPassword, AccessMode accessMode, LnCategoryMask lnCategoryFilter,
        OrchestrationErrorDetail* outDetail, bool* outMmsAvailable, bool* outGooseAvailable) {
    if (sclFilePath) {
        return Orchestration_runFromLocalFile(handle, sclFilePath, host, mmsPort, iedName, interfaceId,
                accessMode, lnCategoryFilter, outDetail, outMmsAvailable, outGooseAvailable);
    }

    LinkedList hostList = LinkedList_create();
    LinkedList_add(hostList, (void*) host);

    OrchestrationError err = Orchestration_run(handle, hostList, mmsPort, iedName, interfaceId,
            accessMode, lnCategoryFilter, outDetail, outMmsAvailable, outGooseAvailable);

    LinkedList_destroyStatic(hostList); /* host is caller-owned, not heap-owned by the list */

    if (err == ORCHESTRATION_ERR_BOOTSTRAP_FAILED
            && outDetail && outDetail->lastCandidateStatus == SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND) {
        err = Orchestration_runFromOnlineDiscovery(handle, host, mmsPort, iedName, interfaceId,
                accessMode, lnCategoryFilter, acseAuthPassword, outDetail, outMmsAvailable, outGooseAvailable);
    }

    return err;
}
