#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_dataset_manager/service/mms_dataset_manager_api.h"
#include "features/mms_dataset_manager/data/mms_dataset_manager_discovery.h"
#include "features/mms_dataset_manager/data/mms_dataset_manager_provisioning.h"
#include "features/mms_dataset_manager/domain/mms_dataset_manager_usecases.h"
#include "features/mms_dataset_manager/utils/mms_dataset_manager_utils.h"

MmsDatasetManagerHandle
MmsDatasetManager_create(IedModelHandle iedModel, IedConnection connection, LinkedList targets,
        MmsDatasetManagerError* outError) {
    if (!iedModel || !connection) {
        if (outError) *outError = MMS_DATASET_MANAGER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    MmsDatasetManagerHandle handle = calloc(1, sizeof(struct sMmsDatasetManagerHandle));
    if (!handle) {
        if (outError) *outError = MMS_DATASET_MANAGER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    handle->iedModel = iedModel;
    handle->connection = connection;
    handle->targets = targets;
    /* domainScopedDynamicDatasetNames stays NULL until the first name
     * actually needs tracking - MmsDatasetManagerNaming_rememberDomainScopedName
     * creates it lazily, and every consumer already treats NULL as "nothing
     * tracked". session is left zeroed by calloc, i.e. inactive. */

    if (outError) *outError = MMS_DATASET_MANAGER_OK;
    return handle;
}

void
MmsDatasetManager_beginCycle(MmsDatasetManagerHandle handle) {
    if (!handle) return;

    /* A caller that reconnects without ending its previous cycle would
     * otherwise silently leak that session's four lists. Ending it here with
     * no orphan cleanup is the honest choice - the previous connection is
     * already gone, so a best-effort delete pass against it would only
     * produce noise. */
    if (handle->session.active) MmsDatasetManager_endCycle(handle, false);

    /* Discovery happens before the session is even seeded, since its result
     * feeds the ConfDataSet budget's own initial value - see
     * MmsDatasetManagerDiscovery_findExistingServerDatasets' and
     * MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget's own doc
     * comments. */
    LinkedList existingServerDatasets = MmsDatasetManagerDiscovery_findExistingServerDatasets(handle);

    /* Fresh per connect cycle, never carried across reconnects - see
     * MmsDatasetManagerSession's own doc comment. Two independent pools - see
     * that same doc comment for why only Conf gets a discovery correction. */
    int sclDynMax = IedModel_getDynDataSetMax(handle->iedModel);
    int sclConfMax = IedModel_getConfDataSetMax(handle->iedModel);
    int existingCount = LinkedList_size(existingServerDatasets);

    handle->session.cache = LinkedList_create();
    handle->session.remainingDynBudget =
            MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(sclDynMax, 0);
    handle->session.dynBudgetExhaustedLogged = false;
    handle->session.remainingConfBudget =
            MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(sclConfMax, existingCount);
    handle->session.confBudgetExhaustedLogged = false;
    handle->session.existingServerDatasets = existingServerDatasets;
    handle->session.claimedDatasetNames = LinkedList_create();
    handle->session.associationSpecificCreateRejected = false;
    handle->session.active = true;
    handle->session.chunkAssignments = MmsDatasetManagerProvisioning_buildWholeDeviceClusterPlan(handle);

    /* One-line, always-on summary of this cycle's real dataset budgets -
     * previously only inferable indirectly, either from the one-shot "budget
     * exhausted" line (which never fires at all if targets simply run out of
     * chunk assignments before the budget itself runs out) or by manually
     * counting per-RCB "resolved via none"/error 99 lines across a whole log
     * capture - exactly the reconstruction this codebase's own real-device
     * investigations have had to do by hand. */
    fprintf(stderr, "[mms_dataset_manager] DynDataSet budget this cycle: SCL declares max=%d, starting budget=%d "
            "(uncorrected - not discoverable ahead of time)\n", sclDynMax, handle->session.remainingDynBudget);
    fprintf(stderr, "[mms_dataset_manager] ConfDataSet budget this cycle: SCL declares max=%d, %d already exist "
            "on server, starting budget=%d\n", sclConfMax, existingCount, handle->session.remainingConfBudget);
}

/* Copies a chunk assignment's own member references into a freshly-owned
 * LinkedList for the caller's decode-shape reconciliation. Returns an EMPTY
 * (not NULL) list for a zero-member chunk - the caller distinguishes the two,
 * see MmsDatasetResolution.memberReferences' own doc comment. */
static LinkedList
copyChunkMemberReferences(const MmsDatasetManagerChunkAssignment* chunk) {
    if (!chunk) return NULL;

    LinkedList list = LinkedList_create();
    if (!list) return NULL;
    for (int i = 0; i < chunk->memberCount; i++) {
        LinkedList_add(list, MmsDatasetManagerUtils_safeStringDup(chunk->memberReferences[i]));
    }
    return list;
}

void
MmsDatasetManager_resolveForTarget(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        ClientReportControlBlock rcb, MmsDatasetResolution* outResolution) {
    if (!outResolution) return;
    memset(outResolution, 0, sizeof(*outResolution));
    outResolution->tier = MMS_DATASET_TIER_NONE;
    outResolution->tierName = "none";
    if (!handle || !target) return;

    /* TIER 1 - STATIC. SCL declared a datSet for this RCB; never touches the
     * network, and the caller already resolved this shape locally at start
     * time from the same immutable value, so no member list is handed back. */
    if (target->datasetReference) {
        outResolution->datasetReference = MmsDatasetManagerUtils_safeStringDup(target->datasetReference);
        outResolution->tier = MMS_DATASET_TIER_SCL;
        outResolution->tierName = "SCL";
    } else {
        LinkedList pulledMemberRefs = NULL;
        /* TIER 2 - PULL LIVE. */
        if (MmsDatasetManagerDiscovery_pullLiveDataset(handle, target, rcb, &pulledMemberRefs)) {
            const char* liveDataset = ClientReportControlBlock_getDataSetReference(rcb);
            outResolution->datasetReference = MmsDatasetManagerUtils_safeStringDup(liveDataset);
            outResolution->memberReferences = pulledMemberRefs;
            outResolution->tier = MMS_DATASET_TIER_LIVE;
            outResolution->tierName = "live";
        } else {
            LinkedList adoptedMemberRefs = NULL;
            /* TIER 3 - ADOPT UNCLAIMED. */
            const char* adopted =
                    MmsDatasetManagerDiscovery_adoptUnclaimedDataset(handle, target, &adoptedMemberRefs);
            if (adopted) {
                outResolution->datasetReference = MmsDatasetManagerUtils_safeStringDup(adopted);
                outResolution->memberReferences = adoptedMemberRefs;
                outResolution->tier = MMS_DATASET_TIER_ADOPTED;
                outResolution->tierName = "adopted";
            } else {
                /* TIER 4 - SELF-CREATE. */
                bool wasNeeded = false;
                const char* created =
                        MmsDatasetManagerProvisioning_getOrCreateDataset(handle, target, &wasNeeded);
                outResolution->datasetReference = created
                        ? MmsDatasetManagerUtils_safeStringDup(created) : NULL;
                outResolution->wasNeeded = wasNeeded;
                outResolution->tier = MMS_DATASET_TIER_SELF_CREATED;
                outResolution->tierName = "self-created";
                /* Only meaningful once a dataset actually resolved - a NULL
                 * created means the caller has nothing to reconcile a shape
                 * against anyway, and its own reconciliation returns early on
                 * a NULL reference regardless. */
                if (created) {
                    outResolution->memberReferences = copyChunkMemberReferences(
                            MmsDatasetManagerProvisioning_findChunkAssignment(&handle->session,
                                    target->objectReference));
                }
            }
        }
    }

    /* NOTE: the per-RCB "dataset resolved via <tier>" summary line is
     * deliberately NOT logged here - the caller emits it AFTER running its own
     * decode-shape reconciliation, so that reconciliation's own
     * "dataset identity changed" line still precedes it in a log capture, as
     * it always has. tierName exists precisely so the caller can do that
     * without a switch of its own. */
}

void
MmsDatasetManager_destroyResolution(MmsDatasetResolution* resolution) {
    if (!resolution) return;

    free(resolution->datasetReference);
    if (resolution->memberReferences) LinkedList_destroyDeep(resolution->memberReferences, free);

    memset(resolution, 0, sizeof(*resolution));
    resolution->tier = MMS_DATASET_TIER_NONE;
    resolution->tierName = "none";
}

void
MmsDatasetManager_endCycle(MmsDatasetManagerHandle handle, bool runOrphanCleanup) {
    if (!handle || !handle->session.active) return;

    int leakedThisCycle = 0;
    if (runOrphanCleanup) MmsDatasetManagerProvisioning_cleanupOrphaned(handle, &leakedThisCycle);
    if (leakedThisCycle > 0) {
        fprintf(stderr, "[mms_dataset_manager] %d domain-scoped dataset(s) could not be reclaimed this cycle "
                "(server denied deletion) - permanently spent against this device's dataset quota until a "
                "device-side reset\n", leakedThisCycle);
    }

    if (handle->session.cache) {
        LinkedList_destroyDeep(handle->session.cache, MmsDatasetManagerProvisioning_destroyCacheEntry);
    }
    if (handle->session.chunkAssignments) {
        LinkedList_destroyDeep(handle->session.chunkAssignments,
                MmsDatasetManagerProvisioning_destroyChunkAssignment);
    }
    if (handle->session.existingServerDatasets) {
        LinkedList_destroyDeep(handle->session.existingServerDatasets, free);
    }
    if (handle->session.claimedDatasetNames) {
        LinkedList_destroyDeep(handle->session.claimedDatasetNames, free);
    }

    memset(&handle->session, 0, sizeof(handle->session));
}

void
MmsDatasetManager_cleanupOnStop(MmsDatasetManagerHandle handle) {
    if (!handle) return;
    MmsDatasetManagerProvisioning_cleanupOnStop(handle);
}

void
MmsDatasetManager_destroy(MmsDatasetManagerHandle handle) {
    if (!handle) return;

    /* Any still-open cycle's session lists are local bookkeeping only -
     * dropping them here never touches the device. No orphan cleanup: the
     * connection may already be closed by the time destroy runs. */
    MmsDatasetManager_endCycle(handle, false);

    /* Ordinarily already NULL - MmsDatasetManager_cleanupOnStop (which the
     * caller is expected to run first, while the connection is still live)
     * deletes every entry server-side and frees this list. Defensive-only
     * fallback for a caller that destroys without stopping first: still frees
     * the local list (no local leak either way), just without the server-side
     * IedConnection_deleteDataSet calls. */
    if (handle->domainScopedDynamicDatasetNames) {
        LinkedList_destroyDeep(handle->domainScopedDynamicDatasetNames, free);
        handle->domainScopedDynamicDatasetNames = NULL;
    }

    free(handle);
}
