#ifndef DEVICE_MANAGER_BOOTSTRAP_POLICY_H_
#define DEVICE_MANAGER_BOOTSTRAP_POLICY_H_

#include "orchestration/service/orchestration_api.h"

/*
 * Extracted from main.c's own pre-device_manager sequencing (see CLAUDE.md's
 * main.c Current-State bullet, pre-multi-device version) so both main.c's own
 * boot-time device and control_dispatcher's worker thread share exactly one
 * copy of this policy instead of duplicating it: if sclFilePath is given,
 * load it directly (iedName is validated as mandatory by the CALLER before
 * this is ever reached - not re-validated here); otherwise attempt network
 * SCL bootstrap, automatically falling back to live online discovery exactly
 * once if bootstrap fails with the specific
 * SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND status (a real, connectable
 * device with no SCL file over MMS file services - see
 * Orchestration_runFromOnlineDiscovery's own doc comment for why this stays
 * an explicit, caller-invoked retry rather than a silent branch inside
 * Orchestration_run itself).
 *
 * Zero third-party includes of its own - only calls orchestration's own
 * public API (which itself transitively brings in LinkedList, used here only
 * to satisfy Orchestration_run's own signature).
 */
OrchestrationError
DeviceManagerBootstrapPolicy_run(OrchestrationHandle handle, const char* host, int mmsPort,
        const char* iedName, const char* interfaceId, const char* sclFilePath,
        const char* acseAuthPassword, AccessMode accessMode, LnCategoryMask lnCategoryFilter,
        OrchestrationErrorDetail* outDetail, bool* outMmsAvailable, bool* outGooseAvailable);

#endif /* DEVICE_MANAGER_BOOTSTRAP_POLICY_H_ */
