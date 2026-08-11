#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_dataset_manager/data/mms_dataset_manager_discovery.h"
#include "features/mms_dataset_manager/data/mms_dataset_manager_naming.h"
#include "features/mms_dataset_manager/domain/mms_dataset_manager_usecases.h"
#include "features/mms_dataset_manager/utils/mms_dataset_manager_utils.h"
#include "log.h"

LinkedList
MmsDatasetManagerDiscovery_findExistingServerDatasets(MmsDatasetManagerHandle handle) {
    LinkedList existing = LinkedList_create();
    if (!handle->targets) return existing;

    LinkedList processedLds = LinkedList_create();

    LinkedList element = LinkedList_getNext(handle->targets);
    while (element) {
        ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);
        element = LinkedList_getNext(element);

        if (target->datasetReference || !target->lnReference) continue;

        char* slash = strchr(target->lnReference, '/');
        if (!slash) continue;
        size_t ldLen = (size_t) (slash - target->lnReference);
        char* ldName = malloc(ldLen + 1);
        if (!ldName) continue;
        memcpy(ldName, target->lnReference, ldLen);
        ldName[ldLen] = '\0';

        if (MmsDatasetManagerNaming_stringListContains(processedLds, ldName)) {
            free(ldName);
            continue;
        }
        LinkedList_add(processedLds, ldName); /* processedLds now owns ldName */

        IedClientError err = IED_ERROR_OK;
        LinkedList mmsNames = IedConnection_getLogicalDeviceDataSets(handle->connection, &err, ldName);
        if (!mmsNames) {
            /* Previously a silent continue - an LD whose directory query is
             * refused contributes nothing to either the budget correction or
             * the adoption pool, which then looks identical to an LD that
             * genuinely holds no datasets at all. */
            SS_LOG_WARN("[mms_dataset_manager] could not list datasets under LD '%s': %s (%d) - this LD "
                    "contributes nothing to the budget correction or the adoption pool\n",
                    ldName, IedClientError_toString(err), err);
            continue;
        }

        int perLdCount = 0;
        LinkedList nameElement = LinkedList_getNext(mmsNames);
        while (nameElement) {
            char* mmsName = (char*) LinkedList_getData(nameElement);
            nameElement = LinkedList_getNext(nameElement);
            if (!mmsName) continue;

            size_t fullLen = strlen(ldName) + 1 + strlen(mmsName) + 1;
            char* full = malloc(fullLen);
            if (!full) continue;
            snprintf(full, fullLen, "%s/%s", ldName, mmsName);
            LinkedList_add(existing, full);
            perLdCount++;
            SS_LOG_DEBUG("[mms_dataset_manager]   existing dataset on server: '%s'\n", full);
        }
        SS_LOG_DEBUG("[mms_dataset_manager] LD '%s' holds %d existing dataset(s)\n", ldName, perLdCount);
        LinkedList_destroyDeep(mmsNames, free);
    }

    LinkedList_destroyDeep(processedLds, free);

    /* Never offer a dataset for adoption that our OWN model already knows is
     * dedicated to a DIFFERENT RCB's SCL-declared static datSet (e.g. an
     * SCL-static buffered RCB's own "ds1") - adopting it wouldn't be
     * incorrect (assignment is non-destructive/shareable), but it would
     * point a Dyn slot at content already fully covered by that other RCB
     * instead of genuinely new device data, undermining the whole point of
     * "primarily use existing datasets" (maximizing distinct coverage, not
     * redundant coverage). Only filters what THIS client's own model already
     * knows about - a dataset some OTHER tool/client has claimed, invisible
     * to us, can't be avoided this way; that's an accepted, honest gap (see
     * this function's own doc comment on scope). */
    LinkedList filterElement = LinkedList_getNext(existing);
    while (filterElement) {
        char* candidate = (char*) LinkedList_getData(filterElement);
        filterElement = LinkedList_getNext(filterElement);

        bool sclKnown = false;
        LinkedList checkElement = LinkedList_getNext(handle->targets);
        while (checkElement) {
            ReportControlBlockTarget* checkTarget = (ReportControlBlockTarget*) LinkedList_getData(checkElement);
            checkElement = LinkedList_getNext(checkElement);
            if (checkTarget->datasetReference && strcmp(checkTarget->datasetReference, candidate) == 0) {
                sclKnown = true;
                break;
            }
        }
        if (sclKnown) {
            SS_LOG_DEBUG("[mms_dataset_manager] excluding '%s' from the adoption pool - our own model says it "
                    "is a DIFFERENT RCB's SCL-declared static datSet, so adopting it would duplicate coverage "
                    "instead of adding any\n", candidate);
            LinkedList_remove(existing, candidate);
            free(candidate);
        }
    }

    SS_LOG_DEBUG("[mms_dataset_manager] discovery complete: %d existing dataset(s) already on the server "
            "across this client's own LD(s), available as adoption candidates and as the ConfDataSet budget "
            "correction\n", LinkedList_size(existing));

    return existing;
}

bool
MmsDatasetManagerDiscovery_pullLiveDataset(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        ClientReportControlBlock rcb, LinkedList* outMemberRefs) {
    MmsDatasetManagerSession* session = &handle->session;
    const char* liveDataset = ClientReportControlBlock_getDataSetReference(rcb);
    /* Both of these rejections used to return silently, which made tier 2
     * indistinguishable from tier 2 never having been consulted at all - the
     * log jumped straight from "resolving" to whatever tier 3/4 did next. */
    if (!liveDataset || liveDataset[0] == '\0') {
        SS_LOG_DEBUG("[mms_dataset_manager] tier 2 (pull live) for '%s': device has no DatSet assigned to this "
                "RCB right now - falling through\n", target->objectReference);
        return false;
    }
    SS_LOG_DEBUG("[mms_dataset_manager] tier 2 (pull live) for '%s': device reports DatSet='%s'\n",
            target->objectReference, liveDataset);
    if (MmsDatasetManagerNaming_looksLikeOurOwnName(liveDataset, target)) {
        SS_LOG_DEBUG("[mms_dataset_manager] tier 2 (pull live) for '%s': '%s' is this client's own "
                "association-scoped name from a PRIOR connection and is already destroyed server-side - "
                "dangling, falling through\n", target->objectReference, liveDataset);
        return false;
    }

    IedClientError err = IED_ERROR_OK;
    bool isDeletable = false;
    LinkedList acsiMembers = IedConnection_getDataSetDirectory(handle->connection, &err, liveDataset, &isDeletable);
    if (!acsiMembers || LinkedList_size(acsiMembers) == 0) {
        SS_LOG_WARN("[mms_dataset_manager] live-assigned dataset '%s' for '%s' could not be resolved: %s (%d) "
                "- will create our own instead\n", liveDataset, target->objectReference,
                IedClientError_toString(err), err);
        if (acsiMembers) LinkedList_destroyDeep(acsiMembers, free);
        return false;
    }

    LinkedList wireRefs = LinkedList_create();
    if (!wireRefs) {
        LinkedList_destroyDeep(acsiMembers, free);
        return false;
    }

    LinkedList element = LinkedList_getNext(acsiMembers);
    while (element) {
        char* acsiRef = (char*) LinkedList_getData(element);
        char* memberRef = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference(acsiRef);
        if (memberRef) {
            LinkedList_add(wireRefs, memberRef);
        } else {
            SS_LOG_WARN("[mms_dataset_manager] could not convert live dataset member '%s' "
                    "(dataset '%s') to wire form - member skipped\n", acsiRef, liveDataset);
        }
        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(acsiMembers, free);

    if (LinkedList_size(wireRefs) == 0) {
        SS_LOG_DEBUG("[mms_dataset_manager] live-assigned dataset '%s' for '%s' had no wire-convertible "
                "members - will create our own instead\n", liveDataset, target->objectReference);
        LinkedList_destroyDeep(wireRefs, free);
        return false;
    }

    SS_LOG_DEBUG("[mms_dataset_manager] reusing live-assigned dataset '%s' for '%s' (%d member(s))\n",
            liveDataset, target->objectReference, LinkedList_size(wireRefs));
    /* The real decode shape every report on this RCB will be interpreted
     * against (the caller rebuilds its member cache from exactly this list) -
     * a mismatch between it and what the device actually sends is the root
     * cause class behind silently misattributed values, and was previously
     * only ever visible as a member COUNT. */
    int pulledIndex = 0;
    int pulledCount = LinkedList_size(wireRefs);
    for (LinkedList pulledElement = LinkedList_getNext(wireRefs); pulledElement;
            pulledElement = LinkedList_getNext(pulledElement)) {
        pulledIndex++;
        SS_LOG_DEBUG("[mms_dataset_manager]   pulled member[%d/%d] of '%s': %s\n",
                pulledIndex, pulledCount, liveDataset, (char*) LinkedList_getData(pulledElement));
    }

    if (session->claimedDatasetNames) {
        LinkedList_add(session->claimedDatasetNames, MmsDatasetManagerUtils_safeStringDup(liveDataset));
    }

    *outMemberRefs = wireRefs;
    return true;
}

/*
 * Resolves one candidate dataset name for adoption: fetches its real member
 * list (IedConnection_getDataSetDirectory) and converts it to this codebase's
 * wire-reference form. Either way - adopted or found unusable - `candidate`
 * is claimed into session->claimedDatasetNames so it isn't re-probed via a
 * wasted wire round-trip for every remaining target this cycle (an unusable
 * candidate is claimed-and-skipped, not retried). Returns true (and fills
 * *outMemberRefs) only if `candidate` was actually adopted for `target`.
 * Factored out of adoptUnclaimedDataset so both its "prefer this target's own
 * name" pass and its general LD-wide scan can share this resolve/claim logic
 * without duplicating it.
 */
static bool
tryAdoptCandidate(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        MmsDatasetManagerSession* session, const char* candidate, LinkedList* outMemberRefs) {
    SS_LOG_DEBUG("[mms_dataset_manager] tier 3 (adopt) for '%s': considering candidate '%s'\n",
            target->objectReference, candidate);

    IedClientError err = IED_ERROR_OK;
    bool isDeletable = false;
    LinkedList acsiMembers = IedConnection_getDataSetDirectory(handle->connection, &err, candidate, &isDeletable);
    if (!acsiMembers || LinkedList_size(acsiMembers) == 0) {
        SS_LOG_WARN("[mms_dataset_manager] discovered dataset '%s' could not be resolved: %s (%d) - "
                "skipping as an adoption candidate\n", candidate, IedClientError_toString(err), err);
        if (acsiMembers) LinkedList_destroyDeep(acsiMembers, free);
        LinkedList_add(session->claimedDatasetNames, MmsDatasetManagerUtils_safeStringDup(candidate));
        return false;
    }

    LinkedList wireRefs = LinkedList_create();
    if (!wireRefs) {
        LinkedList_destroyDeep(acsiMembers, free);
        return false;
    }

    LinkedList acsiElement = LinkedList_getNext(acsiMembers);
    while (acsiElement) {
        char* acsiRef = (char*) LinkedList_getData(acsiElement);
        char* memberRef = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference(acsiRef);
        if (memberRef) LinkedList_add(wireRefs, memberRef);
        acsiElement = LinkedList_getNext(acsiElement);
    }
    LinkedList_destroyDeep(acsiMembers, free);

    if (LinkedList_size(wireRefs) == 0) {
        SS_LOG_DEBUG("[mms_dataset_manager] discovered dataset '%s' had no wire-convertible members - "
                "skipping as an adoption candidate\n", candidate);
        LinkedList_destroyDeep(wireRefs, free);
        LinkedList_add(session->claimedDatasetNames, MmsDatasetManagerUtils_safeStringDup(candidate));
        return false;
    }

    SS_LOG_DEBUG("[mms_dataset_manager] adopting existing dataset '%s' for '%s' (%d member(s), deletable=%d) - "
            "reused instead of self-creating\n", candidate, target->objectReference, LinkedList_size(wireRefs),
            isDeletable);
    /* Same reasoning as pullLiveDataset's own member dump - this list becomes
     * this RCB's decode shape, so it has to be visible when a report decodes
     * to something unexpected. */
    int adoptedIndex = 0;
    int adoptedCount = LinkedList_size(wireRefs);
    for (LinkedList adoptedElement = LinkedList_getNext(wireRefs); adoptedElement;
            adoptedElement = LinkedList_getNext(adoptedElement)) {
        adoptedIndex++;
        SS_LOG_DEBUG("[mms_dataset_manager]   adopted member[%d/%d] of '%s': %s\n",
                adoptedIndex, adoptedCount, candidate, (char*) LinkedList_getData(adoptedElement));
    }

    LinkedList_add(session->claimedDatasetNames, MmsDatasetManagerUtils_safeStringDup(candidate));
    *outMemberRefs = wireRefs;
    return true;
}

const char*
MmsDatasetManagerDiscovery_adoptUnclaimedDataset(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        LinkedList* outMemberRefs) {
    MmsDatasetManagerSession* session = &handle->session;
    if (!session->existingServerDatasets || !target->lnReference) return NULL;

    char* slash = strchr(target->lnReference, '/');
    if (!slash) {
        SS_LOG_WARN("[mms_dataset_manager] tier 3 (adopt) unavailable for '%s': LN reference '%s' has no '/', "
                "so its LD cannot be determined\n", target->objectReference, target->lnReference);
        return NULL;
    }
    size_t ldLen = (size_t) (slash - target->lnReference);

    if (target->buffered && target->objectReference) {
        char* ownName = MmsDatasetManagerNaming_buildDatasetName(target->objectReference, true);
        if (ownName) {
            LinkedList element = LinkedList_getNext(session->existingServerDatasets);
            while (element) {
                char* candidate = (char*) LinkedList_getData(element);
                element = LinkedList_getNext(element);
                if (strcmp(candidate, ownName) != 0) continue;
                if (MmsDatasetManagerNaming_stringListContains(session->claimedDatasetNames, candidate)) {
                    SS_LOG_DEBUG("[mms_dataset_manager] tier 3 (adopt) for '%s': its own previous name '%s' is "
                            "on the server but was already claimed this cycle - falling through to the "
                            "LD-wide scan\n", target->objectReference, candidate);
                    break;
                }
                SS_LOG_DEBUG("[mms_dataset_manager] tier 3 (adopt) for '%s': found its OWN previous dataset "
                        "name '%s' among the discovered candidates - preferring it over the LD-wide scan\n",
                        target->objectReference, candidate);
                if (tryAdoptCandidate(handle, target, session, candidate, outMemberRefs)) {
                    free(ownName);
                    return candidate;
                }
                break;
            }
            free(ownName);
        }
    }

    int consideredCandidates = 0;
    LinkedList element = LinkedList_getNext(session->existingServerDatasets);
    while (element) {
        char* candidate = (char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);

        if (strncmp(candidate, target->lnReference, ldLen) != 0 || candidate[ldLen] != '/') continue;
        consideredCandidates++;
        if (MmsDatasetManagerNaming_stringListContains(session->claimedDatasetNames, candidate)) {
            SS_LOG_DEBUG("[mms_dataset_manager] tier 3 (adopt) for '%s': candidate '%s' already claimed by "
                    "another target this cycle - skipping\n", target->objectReference, candidate);
            continue;
        }

        if (tryAdoptCandidate(handle, target, session, candidate, outMemberRefs)) return candidate;
    }

    SS_LOG_DEBUG("[mms_dataset_manager] tier 3 (adopt) found nothing usable for '%s': %d candidate(s) exist "
            "under its LD, all already claimed or unusable - falling through to self-create\n",
            target->objectReference, consideredCandidates);
    return NULL;
}
