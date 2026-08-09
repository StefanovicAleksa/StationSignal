#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_dataset_manager/data/mms_dataset_manager_provisioning.h"
#include "features/mms_dataset_manager/data/mms_dataset_manager_discovery.h"
#include "features/mms_dataset_manager/data/mms_dataset_manager_naming.h"
#include "features/mms_dataset_manager/domain/mms_dataset_manager_usecases.h"
#include "features/mms_dataset_manager/utils/mms_dataset_manager_utils.h"
#include "mms_client_connection.h"
#include "iec61850_common_internal.h"

void
MmsDatasetManagerProvisioning_destroyCacheEntry(void* entry) {
    if (!entry) return;
    MmsDatasetManagerCacheEntry* e = (MmsDatasetManagerCacheEntry*) entry;
    free(e->lnReference);
    free(e->datasetName);
    free(e);
}

void
MmsDatasetManagerProvisioning_destroyChunkAssignment(void* entry) {
    if (!entry) return;
    MmsDatasetManagerChunkAssignment* e = (MmsDatasetManagerChunkAssignment*) entry;
    free(e->rcbReference);
    if (e->memberReferences) {
        for (int i = 0; i < e->memberCount; i++) free(e->memberReferences[i]);
        free(e->memberReferences);
    }
    free(e);
}

void
MmsDatasetManagerProvisioning_destroyPreResolvedEntry(void* entry) {
    if (!entry) return;
    MmsDatasetManagerPreResolvedEntry* e = (MmsDatasetManagerPreResolvedEntry*) entry;
    free(e->rcbReference);
    free(e->datasetReference);
    if (e->memberReferences) LinkedList_destroyDeep(e->memberReferences, free);
    free(e);
}

MmsDatasetManagerPreResolvedEntry*
MmsDatasetManagerProvisioning_findPreResolvedEntry(LinkedList preResolvedEntries, const char* rcbReference) {
    if (!preResolvedEntries || !rcbReference) return NULL;
    for (LinkedList el = LinkedList_getNext(preResolvedEntries); el; el = LinkedList_getNext(el)) {
        MmsDatasetManagerPreResolvedEntry* entry = (MmsDatasetManagerPreResolvedEntry*) LinkedList_getData(el);
        if (entry->rcbReference && strcmp(entry->rcbReference, rcbReference) == 0) return entry;
    }
    return NULL;
}

void
MmsDatasetManagerProvisioning_runClaimPass(MmsDatasetManagerHandle handle, LinkedList* outClaimedLeaves) {
    LinkedList claimedLeaves = LinkedList_create();
    if (outClaimedLeaves) *outClaimedLeaves = claimedLeaves;

    handle->session.preResolvedEntries = LinkedList_create();
    if (!handle->targets) return;

    int preResolvedCount = 0;
    LinkedList element = LinkedList_getNext(handle->targets);
    while (element) {
        ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);
        element = LinkedList_getNext(element);

        /* SCL-static targets never enter the Dyn pool at all - nothing to
         * pre-resolve, tier 1 already covers them unconditionally in
         * MmsDatasetManager_resolveForTarget. */
        if (target->datasetReference) continue;

        IedClientError err = IED_ERROR_OK;
        ClientReportControlBlock rcb =
                IedConnection_getRCBValues(handle->connection, &err, target->objectReference, NULL);
        if (!rcb) {
            fprintf(stderr, "[mms_dataset_manager] claim pass: getRCBValues failed for '%s': %s (%d) - "
                    "leaving it as a tier-4 (self-create) candidate\n",
                    target->objectReference, IedClientError_toString(err), err);
            continue;
        }

        LinkedList memberRefs = NULL;
        MmsDatasetTier tier = MMS_DATASET_TIER_NONE;
        const char* datasetReference = NULL;

        if (MmsDatasetManagerDiscovery_pullLiveDataset(handle, target, rcb, &memberRefs)) {
            /* Copy BEFORE destroying rcb below - this pointer aliases rcb's
             * own internal MmsValue buffer (ClientReportControlBlock_getDataSetReference
             * returns MmsValue_toString of rcb->datSet), same hazard
             * MmsDatasetResolution.datasetReference's own doc comment
             * describes. */
            datasetReference = ClientReportControlBlock_getDataSetReference(rcb);
            tier = MMS_DATASET_TIER_LIVE;
        } else {
            const char* adopted = MmsDatasetManagerDiscovery_adoptUnclaimedDataset(handle, target, &memberRefs);
            if (adopted) {
                datasetReference = adopted;
                tier = MMS_DATASET_TIER_ADOPTED;
            }
        }

        if (datasetReference) {
            MmsDatasetManagerPreResolvedEntry* entry = malloc(sizeof(MmsDatasetManagerPreResolvedEntry));
            if (entry) {
                entry->rcbReference = MmsDatasetManagerUtils_safeStringDup(target->objectReference);
                entry->datasetReference = MmsDatasetManagerUtils_safeStringDup(datasetReference);
                entry->tier = tier;
                entry->memberReferences = memberRefs;
                LinkedList_add(handle->session.preResolvedEntries, entry);
                preResolvedCount++;

                for (LinkedList m = LinkedList_getNext(memberRefs); m; m = LinkedList_getNext(m)) {
                    char* ref = (char*) LinkedList_getData(m);
                    if (!MmsDatasetManagerNaming_stringListContains(claimedLeaves, ref)) {
                        LinkedList_add(claimedLeaves, MmsDatasetManagerUtils_safeStringDup(ref));
                    }
                }
                fprintf(stderr, "[mms_dataset_manager] claim pass: '%s' pre-resolved via %s: '%s' (%d member(s)) "
                        "- excluded from this cycle's whole-device tier-4 pool\n", target->objectReference,
                        tier == MMS_DATASET_TIER_LIVE ? "live" : "adopted", datasetReference,
                        LinkedList_size(memberRefs));
            } else if (memberRefs) {
                LinkedList_destroyDeep(memberRefs, free);
            }
        }

        ClientReportControlBlock_destroy(rcb);
    }

    fprintf(stderr, "[mms_dataset_manager] claim pass complete: %d target(s) pre-resolved via tier 2/3, %d "
            "distinct leaf(ves) already covered and excluded from this cycle's whole-device tier-4 clustering\n",
            preResolvedCount, LinkedList_size(claimedLeaves));
}

static const char*
lookupDatasetName(LinkedList cache, const char* lnReference, bool buffered) {
    if (!cache || !lnReference) return NULL;

    LinkedList element = LinkedList_getNext(cache);
    while (element) {
        MmsDatasetManagerCacheEntry* entry = (MmsDatasetManagerCacheEntry*) LinkedList_getData(element);
        if (entry->lnReference && entry->buffered == buffered
                && strcmp(entry->lnReference, lnReference) == 0) {
            return entry->datasetName;
        }
        element = LinkedList_getNext(element);
    }
    return NULL;
}

MmsDatasetManagerChunkAssignment*
MmsDatasetManagerProvisioning_findChunkAssignment(MmsDatasetManagerSession* session, const char* rcbReference) {
    if (!session || !session->chunkAssignments || !rcbReference) return NULL;

    LinkedList element = LinkedList_getNext(session->chunkAssignments);
    while (element) {
        MmsDatasetManagerChunkAssignment* entry = (MmsDatasetManagerChunkAssignment*) LinkedList_getData(element);
        if (entry->rcbReference && strcmp(entry->rcbReference, rcbReference) == 0) return entry;
        element = LinkedList_getNext(element);
    }
    return NULL;
}

/* TEMPORARY DIAGNOSTIC - not for production, remove once error 99's real
 * cause is understood (or before ever upgrading past this exact vendored
 * libiec61850 version).
 *
 * IedConnection_createDataSet computes a granular MmsError internally
 * (third_party/include/mms_common.h - decoded straight from the server's
 * real ASN.1 ServiceError PDU, e.g. MMS_ERROR_RESOURCE_CAPABILITY_UNAVAILABLE
 * vs MMS_ERROR_DEFINITION_OBJECT_ATTRIBUTE_INCONSISTENT vs dozens of other
 * distinct reasons) but immediately collapses it to the coarse IedClientError
 * before returning (confirmed against third_party_src/libiec61850's own
 * ied_connection.c:IedConnection_createDataSet, which discards its own local
 * `mmsError` right after computing `*error = iedConnection_mapMmsErrorToIedError(mmsError)`) -
 * IED_ERROR_UNKNOWN/99 in particular is libiec61850's catch-all for whatever
 * it can't map to a more specific IedClientError, so it alone tells us
 * nothing beyond "the device said no."
 *
 * This re-issues the SAME create one level lower, via the same
 * LIB61850_INTERNAL-but-statically-exported symbols
 * IedConnection_createDataSet itself uses under the hood (confirmed present
 * in the vendored archive: `nm third_party/lib/libiec61850.a | grep
 * MmsMapping_ObjectReferenceToVariableAccessSpec` shows a defined, not
 * undefined, symbol) - purely to log MmsError_toString's much more specific
 * answer. Only called on a failure the real attempt already reported; never
 * on the hot/success path.
 *
 * Safety: if this diagnostic call unexpectedly SUCCEEDS where the real one
 * just failed (MMS_ERROR_NONE), the resulting dataset is immediately
 * best-effort deleted rather than left as an untracked orphan on the device -
 * this path must never leave server-side state the rest of this feature
 * doesn't know about, which is exactly the kind of leak the rest of it works
 * hard to avoid. */
static void
logGranularCreateDataSetError(MmsDatasetManagerHandle handle, const char* datasetName,
        const char* const* memberReferences, int memberCount, bool buffered) {
    MmsConnection mmsConnection = IedConnection_getMmsConnection(handle->connection);
    if (!mmsConnection) return;

    LinkedList wireRefs = MmsDatasetManagerUseCases_buildWireMemberReferences(memberReferences, memberCount);
    if (!wireRefs) return;

    LinkedList specs = LinkedList_create();
    LinkedList element = LinkedList_getNext(wireRefs);
    while (element) {
        MmsVariableAccessSpecification* spec =
                MmsMapping_ObjectReferenceToVariableAccessSpec((char*) LinkedList_getData(element));
        if (spec) LinkedList_add(specs, spec);
        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(wireRefs, free);

    MmsError mmsError = MMS_ERROR_NONE;
    bool attempted = false;

    if (buffered) {
        const char* slash = strchr(datasetName, '/');
        if (slash) {
            char domainId[65];
            size_t domainLen = (size_t) (slash - datasetName);
            if (domainLen < sizeof(domainId)) {
                memcpy(domainId, datasetName, domainLen);
                domainId[domainLen] = '\0';
                MmsConnection_defineNamedVariableList(mmsConnection, &mmsError, domainId, slash + 1, specs);
                attempted = true;
            }
        }
    } else if (datasetName[0] == '@') {
        MmsConnection_defineNamedVariableListAssociationSpecific(mmsConnection, &mmsError, datasetName + 1, specs);
        attempted = true;
    }

    if (attempted) {
        fprintf(stderr, "[mms_dataset_manager] createDataSet diagnostic for '%s': raw MMS error = %s (%d)\n",
                datasetName, MmsError_toString(mmsError), (int) mmsError);

        if (mmsError == MMS_ERROR_NONE) {
            fprintf(stderr, "[mms_dataset_manager] createDataSet diagnostic for '%s' UNEXPECTEDLY SUCCEEDED where "
                    "the real attempt just failed - deleting it now rather than leaving an untracked orphan on "
                    "the device\n", datasetName);
            IedClientError deleteErr = IED_ERROR_OK;
            if (!IedConnection_deleteDataSet(handle->connection, &deleteErr, datasetName)) {
                fprintf(stderr, "[mms_dataset_manager] could not delete diagnostic dataset '%s': %s (%d) - left "
                        "behind on the device\n", datasetName, IedClientError_toString(deleteErr), deleteErr);
            }
        }
    }

    LinkedList_destroyDeep(specs, (LinkedListValueDeleteFunction) MmsVariableAccessSpecification_destroy);
}

/*
 * Shared createDataSet + cache-insert + budget-decrement plumbing:
 * `cacheKey` is always target->objectReference (already unique per target,
 * doubling as the naming/cache key with no separate scheme needed) and
 * `memberReferences`/`memberCount` are this target's own whole-device cluster
 * assignment. `logLnReference`/`logRcbReference` are for messages only.
 */
static const char*
createAndCacheDatasetAttempt(MmsDatasetManagerHandle handle, MmsDatasetManagerSession* session, const char* cacheKey,
        const char* logLnReference, const char* logRcbReference, const char* const* memberReferences,
        int memberCount, bool buffered) {
    /* Which pool this attempt draws from is exactly what `buffered` already
     * means here - see MmsDatasetManagerSession's own doc comment. Checked
     * before any wire work so an already-exhausted pool costs nothing beyond
     * this one log line (edge-triggered, see that same doc comment). */
    int* budget = buffered ? &session->remainingConfBudget : &session->remainingDynBudget;
    bool* budgetExhaustedLogged = buffered ? &session->confBudgetExhaustedLogged : &session->dynBudgetExhaustedLogged;

    /* Entry banner. Every input this attempt depends on, in one place, before
     * anything can fail - so a rejection further down can be read against the
     * exact request that produced it rather than reconstructed from
     * surrounding lines. */
    fprintf(stderr, "[mms_dataset_manager] === createDataSet attempt for '%s' (LN '%s') === scope=%s, "
            "requested members=%d, pool=%s, remaining budget=%d\n",
            logRcbReference, logLnReference, buffered ? "domain-scoped" : "association-specific",
            memberCount, buffered ? "ConfDataSet" : "DynDataSet", *budget);

    if (MmsDatasetManagerUseCases_isDynamicDatasetBudgetExhausted(*budget)) {
        if (!*budgetExhaustedLogged) {
            int sclMax = buffered ? IedModel_getConfDataSetMax(handle->iedModel)
                                   : IedModel_getDynDataSetMax(handle->iedModel);
            fprintf(stderr, "[mms_dataset_manager] %s budget (SCL %s max=%d) exhausted this connect cycle - no "
                    "further createDataSet attempts will be made; remaining RCB(s) needing a %s dataset will "
                    "not report\n", buffered ? "ConfDataSet" : "DynDataSet", buffered ? "ConfDataSet" : "DynDataSet",
                    sclMax, buffered ? "domain-scoped" : "association-specific");
            *budgetExhaustedLogged = true;
        }
        return NULL;
    }

    /* Every member reference this attempt was ASKED to include, before any
     * conversion - the input side of the comparison against the wire form
     * below. Verbose on purpose: on a device rejecting a create for reasons
     * it won't name (IED_ERROR_UNKNOWN/99), the exact member list is the only
     * thing left to reason about. */
    for (int i = 0; i < memberCount; i++) {
        fprintf(stderr, "[mms_dataset_manager]   requested member[%d/%d] for '%s': %s\n",
                i + 1, memberCount, logRcbReference, memberReferences[i] ? memberReferences[i] : "(null)");
    }

    LinkedList wireRefs = MmsDatasetManagerUseCases_buildWireMemberReferences(memberReferences, memberCount);
    if (!wireRefs || LinkedList_size(wireRefs) == 0) {
        fprintf(stderr, "[mms_dataset_manager] no wire-convertible attribute references for LN '%s' (0 of %d "
                "converted) - '%s' will not get a dynamic dataset\n", logLnReference, memberCount,
                logRcbReference);
        if (wireRefs) LinkedList_destroyDeep(wireRefs, free);
        return NULL;
    }

    /* A PARTIAL conversion loss silently changes the dataset's shape - the
     * created dataset genuinely has fewer members than this target's cluster
     * assignment believes, which then has to be reconciled by the caller
     * against a member list that no longer matches. Previously only the
     * total-loss case above said anything at all, so a partial drop left no
     * trace whatsoever. */
    int wireCount = LinkedList_size(wireRefs);
    if (wireCount != memberCount) {
        fprintf(stderr, "[mms_dataset_manager] WARNING: converted only %d of %d member reference(s) to wire form "
                "for '%s' - %d dropped; the created dataset's real shape will differ from this target's own "
                "cluster assignment\n", wireCount, memberCount, logRcbReference, memberCount - wireCount);
    }

    char* datasetName = MmsDatasetManagerNaming_buildDatasetName(cacheKey, buffered);
    if (!datasetName) {
        LinkedList_destroyDeep(wireRefs, free);
        return NULL;
    }

    /* Logged before the call, not just on failure - this member count is the
     * single most useful number for diagnosing a device's maxAttributes-cap
     * rejection (e.g. libiec61850's IED_ERROR_UNKNOWN/99, seen against a real
     * SIPROTEC 6MD663 declaring DynDataSet maxAttributes="60") - without it, a
     * failure below only shows a raw error code with nothing to compare it
     * against. */
    fprintf(stderr, "[mms_dataset_manager] creating dynamic dataset '%s' for LN '%s' with %d member attribute(s) "
            "(as actually sent on the wire)\n", datasetName, logLnReference, wireCount);
    int wireIndex = 0;
    for (LinkedList wireElement = LinkedList_getNext(wireRefs); wireElement;
            wireElement = LinkedList_getNext(wireElement)) {
        wireIndex++;
        fprintf(stderr, "[mms_dataset_manager]   wire member[%d/%d] of '%s': %s\n",
                wireIndex, wireCount, datasetName, (char*) LinkedList_getData(wireElement));
    }

    IedClientError err = IED_ERROR_OK;
    IedConnection_createDataSet(handle->connection, &err, datasetName, wireRefs);
    LinkedList_destroyDeep(wireRefs, free);

    /* Domain-scoped names are deterministic and persist past a connection
     * (unlike the "@"-scoped ones), so a reconnect - or a prior daemon run
     * that never got to clean up - can legitimately find this exact name
     * already on the server. Treat that as a successful reuse, not a
     * failure: the member list is derived the same deterministic way every
     * time (this target's own chunk), so an existing dataset by this name is
     * presumed to already have the right shape. */
    bool reusedExisting = false;
    if (err == IED_ERROR_OBJECT_EXISTS && buffered) {
        fprintf(stderr, "[mms_dataset_manager] dynamic dataset '%s' for LN '%s' already exists on the server - "
                "reusing it (budget unchanged at %d, it was never new this cycle)\n",
                datasetName, logLnReference, *budget);
        reusedExisting = true;
    } else if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_dataset_manager] dynamic dataset creation FAILED for LN '%s' (dataset '%s', %d "
                "member(s), %s): %s (%d) - '%s' will not report\n", logLnReference, datasetName, wireCount,
                buffered ? "domain-scoped" : "association-specific", IedClientError_toString(err), err,
                logRcbReference);
        logGranularCreateDataSetError(handle, datasetName, memberReferences, memberCount, buffered);
        free(datasetName);
        return NULL;
    }

    if (!reusedExisting) {
        /* Only a genuinely NEW dataset counts against this cycle's budget -
         * a reused pre-existing one was never new against the device's own
         * count this cycle (see MmsDatasetManagerSession's own doc comment on
         * why cache hits stay free; this is the same reasoning applied to a
         * dataset the SERVER already knew about before this cycle started). */
        if (*budget > 0) (*budget)--;
        fprintf(stderr, "[mms_dataset_manager] created dynamic dataset '%s' for LN '%s' with %d member(s) - "
                "%s budget now %d\n", datasetName, logLnReference, wireCount,
                buffered ? "ConfDataSet" : "DynDataSet", *budget);
    }

    if (buffered) MmsDatasetManagerNaming_rememberDomainScopedName(handle, datasetName);

    MmsDatasetManagerCacheEntry* cacheNode = malloc(sizeof(MmsDatasetManagerCacheEntry));
    if (!cacheNode) {
        /* Dataset now exists on the server but there's no way to remember
         * the name for reuse this cycle - association-scoped, so it's
         * cleaned up automatically when this connection eventually closes;
         * just missing a reuse opportunity for the rest of this cycle, not a
         * leak. Domain-scoped (buffered) names are still safe too - already
         * captured in handle->domainScopedDynamicDatasetNames above for
         * eventual cleanup regardless of this cache node's fate. */
        free(datasetName);
        return NULL;
    }
    cacheNode->lnReference = MmsDatasetManagerUtils_safeStringDup(cacheKey);
    cacheNode->datasetName = datasetName;
    cacheNode->buffered = buffered;
    LinkedList_add(session->cache, cacheNode);

    return cacheNode->datasetName;
}

/* Some real devices (confirmed against a real SIPROTEC 6MD: every
 * association-specific create rejected with MMS reject-other, not a
 * resource/quota-class ServiceError, independent of remaining budget - see
 * CHANGELOG.md) reject association-specific ("@"-prefixed) dataset creation
 * outright, structurally, regardless of capacity. A domain-scoped dataset
 * assigned to an unbuffered RCB is otherwise perfectly valid IEC 61850 (this
 * feature already does exactly that via the ADOPT tier whenever an existing
 * one is found) - only the CREATION form was ever the problem for such a
 * device, not the assignment. So an unbuffered target whose real
 * association-specific attempt fails gets one fallback attempt as a
 * domain-scoped dataset instead, accepting the same lifecycle tradeoff a
 * buffered target's dataset already has: it persists past this connection
 * and won't be auto-cleaned by the server, so it's tracked via
 * MmsDatasetManagerNaming_rememberDomainScopedName exactly like a buffered
 * target's own (createAndCacheDatasetAttempt keys every naming/reuse/cleanup
 * decision off its own `buffered` parameter, never off the target's real
 * buffered-ness, so calling it a second time with buffered=true here "just
 * works," including OBJECT_EXISTS-as-reuse on a later reconnect once this
 * fallback dataset already exists on the server).
 *
 * A buffered target already always uses the domain-scoped path on its one
 * and only attempt - `buffered` short-circuits below because there is no
 * "more scoped" fallback left to try.
 *
 * Once that fallback has actually worked once this cycle, the association-
 * specific attempt is skipped for every remaining unbuffered target
 * (session->associationSpecificCreateRejected). On a device that rejects the
 * form structurally it never succeeds for ANY target, so retrying it per
 * target only spends a doomed ~100-member createDataSet round-trip each time -
 * measured at nine wasted large PDUs in one real connect cycle, against a
 * device this codebase already works hard not to hammer. The latch is per
 * connect cycle and never persisted, so nothing permanently gives up on the
 * cheaper association-specific form. */
static const char*
createAndCacheDataset(MmsDatasetManagerHandle handle, MmsDatasetManagerSession* session, const char* cacheKey,
        const char* logLnReference, const char* logRcbReference, const char* const* memberReferences,
        int memberCount, bool buffered) {
    if (!buffered && session->associationSpecificCreateRejected) {
        fprintf(stderr, "[mms_dataset_manager] skipping the association-specific createDataSet attempt for LN "
                "'%s' - this device already rejected that form earlier this cycle, so going straight to a "
                "domain-scoped dataset instead of spending another doomed %d-member round-trip\n",
                logLnReference, memberCount);
        return createAndCacheDatasetAttempt(handle, session, cacheKey, logLnReference, logRcbReference,
                memberReferences, memberCount, true);
    }

    const char* result = createAndCacheDatasetAttempt(handle, session, cacheKey, logLnReference,
            logRcbReference, memberReferences, memberCount, buffered);
    if (result || buffered) return result;

    fprintf(stderr, "[mms_dataset_manager] association-specific dataset creation failed for LN '%s' - falling "
            "back to a domain-scoped dataset instead (some real devices reject association-specific creation "
            "outright, independent of quota - see CHANGELOG.md) - this dataset will NOT be cleaned up "
            "automatically and permanently consumes device quota until a device-side reset\n", logLnReference);
    const char* fallback = createAndCacheDatasetAttempt(handle, session, cacheKey, logLnReference,
            logRcbReference, memberReferences, memberCount, true);

    /* Latch only on a fallback that actually WORKED. If the domain-scoped
     * attempt failed too, the association-specific form isn't demonstrably the
     * problem (both pools may simply be exhausted), so nothing is concluded and
     * the next target retries normally. */
    if (fallback && !session->associationSpecificCreateRejected) {
        session->associationSpecificCreateRejected = true;
        fprintf(stderr, "[mms_dataset_manager] this device rejects association-specific createDataSet but "
                "accepts domain-scoped - remaining unbuffered targets this cycle will skip the "
                "association-specific attempt entirely\n");
    }
    return fallback;
}

const char*
MmsDatasetManagerProvisioning_getOrCreateDataset(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        bool* outWasNeeded) {
    MmsDatasetManagerSession* session = &handle->session;
    *outWasNeeded = false;

    if (!session->active || !target->lnReference) {
        /* A target with no LN reference is a malformed model, not a spare
         * slot the device happens not to need - genuinely a failure. */
        *outWasNeeded = true;
        fprintf(stderr, "[mms_dataset_manager] tier 4 (self-create) unavailable for '%s': %s\n",
                target->objectReference, session->active ? "target has no LN reference" : "no dataset session");
        return NULL;
    }

    MmsDatasetManagerChunkAssignment* chunk =
            MmsDatasetManagerProvisioning_findChunkAssignment(session, target->objectReference);
    if (!chunk) {
        /* Not an error - whole-device clustering ran out of clusters before
         * it ran out of Dyn RCB slots, i.e. the device's reportable data is
         * already fully covered by other targets this cycle (see
         * buildWholeDeviceClusterPlan's own underflow log). Stated explicitly
         * so it reads as "nothing left to cover" instead of looking like a
         * lookup bug, and reported via *outWasNeeded=false so the caller
         * leaves this RCB alone entirely rather than calling it failed. */
        fprintf(stderr, "[mms_dataset_manager] tier 4 (self-create) skipped for '%s': no whole-device cluster was "
                "assigned to this RCB slot this cycle (the device's reportable data is already fully covered "
                "by other slots)\n", target->objectReference);
        return NULL;
    }
    fprintf(stderr, "[mms_dataset_manager] tier 4 (self-create) for '%s': assigned cluster has %d member(s)\n",
            target->objectReference, chunk->memberCount);

    const char* existing = lookupDatasetName(session->cache, target->objectReference, target->buffered);
    if (existing) {
        fprintf(stderr, "[mms_dataset_manager] tier 4 (self-create) for '%s': session cache hit - reusing '%s' "
                "already created this cycle, no wire call\n", target->objectReference, existing);
        return existing;
    }

    /* Cache hits above stay free (zero wire cost, always allowed) - the
     * budget check itself lives in createAndCacheDatasetAttempt, keyed
     * per-attempt on which pool that attempt actually draws from (see
     * MmsDatasetManagerSession's own doc comment) - a single top-level check
     * here couldn't distinguish an unbuffered target's Dyn-budget-gated real
     * attempt from its own Conf-budget-gated fallback. */
    const char* created = createAndCacheDataset(handle, session, target->objectReference,
            target->lnReference, target->objectReference, (const char* const*) chunk->memberReferences,
            chunk->memberCount, target->buffered);
    /* Past the chunk lookup above, this target demonstrably HAD work to do,
     * so a NULL here is a genuine failure to do it - never the benign
     * "nothing left to cover" case. */
    if (!created) *outWasNeeded = true;
    return created;
}

LinkedList
MmsDatasetManagerProvisioning_buildWholeDeviceClusterPlan(MmsDatasetManagerHandle handle, LinkedList claimedLeaves) {
    LinkedList plan = LinkedList_create();
    if (!handle->targets) return plan;

    /* Slot pool: every Dyn target on the WHOLE device that did NOT already
     * resolve via MmsDatasetManagerProvisioning_runClaimPass's own tier 2/3
     * claim pass (handle->session.preResolvedEntries - see that function's
     * own doc comment), in handle->targets' own existing order - a list of
     * BORROWED ReportControlBlockTarget* (owned by handle->targets itself),
     * destroyed with LinkedList_destroyStatic below, never Deep. Unlike the
     * old per-LN chunk plan, this is not filtered/grouped by LN at all -
     * every eligible Dyn target anywhere on the device is one fungible slot
     * in one combined pool. */
    LinkedList slots = LinkedList_create();
    LinkedList slotScan = LinkedList_getNext(handle->targets);
    while (slotScan) {
        ReportControlBlockTarget* t = (ReportControlBlockTarget*) LinkedList_getData(slotScan);
        if (!t->datasetReference
                && !MmsDatasetManagerProvisioning_findPreResolvedEntry(handle->session.preResolvedEntries,
                        t->objectReference)) {
            LinkedList_add(slots, t);
        }
        slotScan = LinkedList_getNext(slotScan);
    }
    if (LinkedList_size(slots) == 0) {
        fprintf(stderr, "[mms_dataset_manager] whole-device cluster plan: this device has NO Dyn RCB slots (every "
                "RCB already carries an SCL-declared datSet) - nothing to plan\n");
        LinkedList_destroyStatic(slots);
        return plan;
    }
    fprintf(stderr, "[mms_dataset_manager] whole-device cluster plan: %d Dyn RCB slot(s) available\n",
            LinkedList_size(slots));
    for (LinkedList slotLog = LinkedList_getNext(slots); slotLog; slotLog = LinkedList_getNext(slotLog)) {
        ReportControlBlockTarget* t = (ReportControlBlockTarget*) LinkedList_getData(slotLog);
        fprintf(stderr, "[mms_dataset_manager]   Dyn slot: '%s' (LN '%s', buffered=%d)\n",
                t->objectReference, t->lnReference ? t->lnReference : "(none)", t->buffered);
    }

    LinkedList wholeDeviceLeaves = IedModel_getReportableAttributeReferencesForWholeDevice(handle->iedModel);
    int rawLeafCount = wholeDeviceLeaves ? LinkedList_size(wholeDeviceLeaves) : 0;
    if (rawLeafCount == 0) {
        fprintf(stderr, "[mms_dataset_manager] whole-device cluster plan: the model has NO reportable (FC=ST/MX) "
                "attributes anywhere - no Dyn RCB can be given a dataset this cycle\n");
        if (wholeDeviceLeaves) LinkedList_destroyDeep(wholeDeviceLeaves, free);
        LinkedList_destroyStatic(slots);
        return plan;
    }
    fprintf(stderr, "[mms_dataset_manager] whole-device cluster plan: %d reportable attribute(s) across the "
            "whole device (before excluding what the claim pass already covered)\n", rawLeafCount);

    char** rawLeafArray = calloc((size_t) rawLeafCount, sizeof(char*));
    if (!rawLeafArray) {
        LinkedList_destroyDeep(wholeDeviceLeaves, free);
        LinkedList_destroyStatic(slots);
        return plan;
    }
    int rawLeafIdx = 0;
    for (LinkedList el = LinkedList_getNext(wholeDeviceLeaves); el; el = LinkedList_getNext(el)) {
        rawLeafArray[rawLeafIdx++] = (char*) LinkedList_getData(el);
    }

    /* Exclude whatever MmsDatasetManagerProvisioning_runClaimPass's own tier
     * 2/3 pass already covered this cycle - see this function's own doc
     * comment for why. unclaimedLeaves owns its own duplicated strings,
     * independent of wholeDeviceLeaves, which is freed right after. */
    LinkedList unclaimedLeaves = MmsDatasetManagerUseCases_filterOutClaimedLeaves(
            (const char* const*) rawLeafArray, rawLeafCount, claimedLeaves);
    free(rawLeafArray);
    LinkedList_destroyDeep(wholeDeviceLeaves, free);

    int leafCount = LinkedList_size(unclaimedLeaves);
    fprintf(stderr, "[mms_dataset_manager] whole-device cluster plan: %d attribute(s) already covered by this "
            "cycle's tier 2/3 claim pass excluded - %d remain to distribute across %d Dyn slot(s)\n",
            rawLeafCount - leafCount, leafCount, LinkedList_size(slots));
    if (leafCount == 0) {
        fprintf(stderr, "[mms_dataset_manager] whole-device cluster plan: nothing left to cluster - the "
                "device's reportable data is already fully covered by this cycle's tier 2/3 claims\n");
        LinkedList_destroyStatic(unclaimedLeaves);
        LinkedList_destroyStatic(slots);
        return plan;
    }

    char** leafArray = calloc((size_t) leafCount, sizeof(char*));
    if (!leafArray) {
        LinkedList_destroyDeep(unclaimedLeaves, free);
        LinkedList_destroyStatic(slots);
        return plan;
    }
    int leafIdx = 0;
    for (LinkedList el = LinkedList_getNext(unclaimedLeaves); el; el = LinkedList_getNext(el)) {
        leafArray[leafIdx++] = (char*) LinkedList_getData(el);
    }

    /* Prefer ConfDataSet's own declared maxAttributes over DynDataSet's - see
     * this function's own doc comment for why. Falls back to DynDataSet's if
     * Conf isn't declared, then to per-LN grouping (below) if neither is. */
    int confMaxAttributes = IedModel_getConfDataSetMaxAttributes(handle->iedModel);
    int dynMaxAttributes = IedModel_getDynDataSetMaxAttributes(handle->iedModel);
    int maxAttributes = confMaxAttributes > 0 ? confMaxAttributes : dynMaxAttributes;
    fprintf(stderr, "[mms_dataset_manager] whole-device cluster plan: SCL declares ConfDataSet maxAttributes=%d, "
            "DynDataSet maxAttributes=%d - using cap %d from %s, strategy=%s\n",
            confMaxAttributes, dynMaxAttributes, maxAttributes,
            confMaxAttributes > 0 ? "ConfDataSet" : (dynMaxAttributes > 0 ? "DynDataSet" : "neither (undeclared)"),
            maxAttributes > 0 ? "DO-atomic bin-packing across the whole device"
                              : "one dataset per LN (no known size bound to pack against)");
    LinkedList clusterLists = (maxAttributes > 0)
            ? MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(
                    (const char* const*) leafArray, leafCount, maxAttributes)
            : MmsDatasetManagerUseCases_groupReferencesByLn((const char* const*) leafArray, leafCount);
    free(leafArray);
    LinkedList_destroyDeep(unclaimedLeaves, free);

    int totalClusters = LinkedList_size(clusterLists);
    int slotCount = LinkedList_size(slots);
    int assignedClusters = 0;
    fprintf(stderr, "[mms_dataset_manager] whole-device cluster plan: chunking produced %d cluster(s) for %d "
            "slot(s)\n", totalClusters, slotCount);

    LinkedList clusterElement = LinkedList_getNext(clusterLists);
    LinkedList slotElement = LinkedList_getNext(slots);
    while (clusterElement) {
        if (!slotElement) {
            fprintf(stderr, "[mms_dataset_manager] whole-device clustering produced %d dataset(s) but this "
                    "device only has %d RCB instance(s) with no SCL-assigned dataset - %d cluster(s) "
                    "(part of the device's data) will not be reported this cycle\n",
                    totalClusters, slotCount, totalClusters - assignedClusters);
            break;
        }

        LinkedList clusterMembers = (LinkedList) LinkedList_getData(clusterElement);
        ReportControlBlockTarget* assignedTarget = (ReportControlBlockTarget*) LinkedList_getData(slotElement);

        int memberCount = LinkedList_size(clusterMembers);
        char** memberArray = calloc((size_t) memberCount, sizeof(char*));
        if (memberArray) {
            int m = 0;
            for (LinkedList mEl = LinkedList_getNext(clusterMembers); mEl; mEl = LinkedList_getNext(mEl)) {
                memberArray[m++] = MmsDatasetManagerUtils_safeStringDup((char*) LinkedList_getData(mEl));
            }

            MmsDatasetManagerChunkAssignment* assignment = malloc(sizeof(MmsDatasetManagerChunkAssignment));
            if (assignment) {
                assignment->rcbReference = MmsDatasetManagerUtils_safeStringDup(assignedTarget->objectReference);
                assignment->memberReferences = memberArray;
                assignment->memberCount = memberCount;
                LinkedList_add(plan, assignment);
                /* The plan itself, one line per assignment. Which slot got
                 * which part of the device is otherwise only inferable by
                 * correlating createDataSet lines after the fact, and a
                 * cluster whose size exceeds the device's real (as opposed to
                 * SCL-declared) cap is only diagnosable if the intended size
                 * is on record before the create is attempted. */
                fprintf(stderr, "[mms_dataset_manager]   cluster %d/%d -> slot '%s' (%d member(s))\n",
                        assignedClusters + 1, totalClusters, assignedTarget->objectReference, memberCount);
            } else {
                for (int f = 0; f < memberCount; f++) free(memberArray[f]);
                free(memberArray);
            }
        }

        assignedClusters++;
        clusterElement = LinkedList_getNext(clusterElement);
        slotElement = LinkedList_getNext(slotElement);
    }

    if (!clusterElement && slotElement) {
        fprintf(stderr, "[mms_dataset_manager] whole-device clustering produced %d dataset(s) for %d "
                "available RCB instance(s) - %d instance(s) have nothing left to report (the device's "
                "own reportable data is already fully covered)\n",
                totalClusters, slotCount, slotCount - totalClusters);
    }

    LinkedList clusterCleanup = LinkedList_getNext(clusterLists);
    while (clusterCleanup) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(clusterCleanup), free);
        clusterCleanup = LinkedList_getNext(clusterCleanup);
    }
    LinkedList_destroyStatic(clusterLists);
    LinkedList_destroyStatic(slots);

    return plan;
}

/* True if `datasetName` is the resolved dataset name of any entry already in
 * `cache` - used by cleanupOrphaned to recognize a discovered existing
 * dataset that a target already actively claimed/reused this cycle (via tier
 * 4's own create-or-reuse-on-exists path), as distinct from one the adopt
 * tier separately tracks in session->claimedDatasetNames. */
static bool
cacheContainsDatasetName(LinkedList cache, const char* datasetName) {
    if (!cache || !datasetName) return false;
    LinkedList element = LinkedList_getNext(cache);
    while (element) {
        MmsDatasetManagerCacheEntry* entry = (MmsDatasetManagerCacheEntry*) LinkedList_getData(element);
        if (entry->datasetName && strcmp(entry->datasetName, datasetName) == 0) return true;
        element = LinkedList_getNext(element);
    }
    return false;
}

void
MmsDatasetManagerProvisioning_cleanupOrphaned(MmsDatasetManagerHandle handle, int* outLeakedCount) {
    MmsDatasetManagerSession* session = &handle->session;
    if (!session->existingServerDatasets || !handle->targets) return;

    fprintf(stderr, "[mms_dataset_manager] orphan cleanup: evaluating %d dataset(s) discovered on the server "
            "this cycle\n", LinkedList_size(session->existingServerDatasets));

    LinkedList element = LinkedList_getNext(session->existingServerDatasets);
    while (element) {
        char* candidate = (char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);

        if (MmsDatasetManagerNaming_stringListContains(session->claimedDatasetNames, candidate)) continue;
        if (cacheContainsDatasetName(session->cache, candidate)) continue;

        bool isOurs = false;
        ReportControlBlockTarget* matchedTarget = NULL;
        LinkedList targetElement = LinkedList_getNext(handle->targets);
        while (targetElement) {
            ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(targetElement);
            targetElement = LinkedList_getNext(targetElement);
            if (!target->buffered || !target->objectReference) continue;

            char* expected = MmsDatasetManagerNaming_buildDatasetName(target->objectReference, true);
            bool match = expected && strcmp(expected, candidate) == 0;
            free(expected);
            if (match) {
                isOurs = true;
                matchedTarget = target;
                break;
            }
        }
        if (!isOurs) continue;

        /* Same fix as MmsDatasetManagerProvisioning_cleanupOnStop's own
         * graceful cleanup: disabling RptEna alone is not enough to unblock
         * deletion (confirmed against the reference server -
         * IED_ERROR_OBJECT_CONSTRAINT_CONFLICT/35, see
         * test_deleteDataSet_refusedWhileRcbEnabled_succeedsAfterDisable) -
         * the RCB's own DatSet attribute continuing to point at this orphan
         * is itself the constraint. A crashed daemon never reached
         * cleanupOnStop's own disable+unbind at all, so matchedTarget's
         * RptEna/DatSet may genuinely still be live server-side here exactly
         * as if this were the graceful path - this proactive pass needs the
         * identical two-step sequence before its own delete attempt below.
         * Scoped the same way too: only touches matchedTarget's RCB if its
         * CURRENT live DatSet actually is this candidate, never blind. */
        IedClientError disableErr = IED_ERROR_OK;
        ClientReportControlBlock orphanRcb = IedConnection_getRCBValues(handle->connection, &disableErr,
                matchedTarget->objectReference, NULL);
        if (orphanRcb) {
            const char* liveDataSet = ClientReportControlBlock_getDataSetReference(orphanRcb);
            if (liveDataSet && strcmp(liveDataSet, candidate) == 0) {
                ClientReportControlBlock_setRptEna(orphanRcb, false);
                ClientReportControlBlock_setDataSetReference(orphanRcb, "");
                IedConnection_setRCBValues(handle->connection, &disableErr, orphanRcb,
                        RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_DATSET, true);
                if (disableErr != IED_ERROR_OK) {
                    fprintf(stderr, "[mms_dataset_manager] could not disable/unbind '%s' (dataset '%s') "
                            "before orphan cleanup: %s (%d)\n", matchedTarget->objectReference, candidate,
                            IedClientError_toString(disableErr), disableErr);
                } else {
                    fprintf(stderr, "[mms_dataset_manager] disabled/unbound '%s' from orphaned dataset '%s' "
                            "before cleanup\n", matchedTarget->objectReference, candidate);
                }
            } else {
                fprintf(stderr, "[mms_dataset_manager] orphan candidate '%s' matches '%s's own deterministic "
                        "name but its live DatSet is '%s' - not currently bound to it, skipping disable/"
                        "unbind\n", candidate, matchedTarget->objectReference, liveDataSet ? liveDataSet : "(none)");
            }
            ClientReportControlBlock_destroy(orphanRcb);
        }

        IedClientError deleteErr = IED_ERROR_OK;
        if (IedConnection_deleteDataSet(handle->connection, &deleteErr, candidate)) {
            fprintf(stderr, "[mms_dataset_manager] cleaned up orphaned dataset '%s' - not needed by any "
                    "target this cycle, reclaiming budget\n", candidate);
        } else {
            fprintf(stderr, "[mms_dataset_manager] could not clean up orphaned dataset '%s': %s (%d) - "
                    "left behind\n", candidate, IedClientError_toString(deleteErr), deleteErr);
            if (outLeakedCount) (*outLeakedCount)++;
        }
    }
}

void
MmsDatasetManagerProvisioning_cleanupOnStop(MmsDatasetManagerHandle handle) {
    /* Confirmed empirically (manual IEDScout testing against a real device,
     * then proven end-to-end against the reference server too - see
     * integration_tests/mms_report_client's test_deleteDataSet_refusedWhileRcbEnabled_succeedsAfterDisable):
     * a dataset still referenced by an RCB's DatSet is refused for deletion
     * regardless of RptEna. Disabling RptEna alone is NOT enough - the
     * reference server still refused with IED_ERROR_OBJECT_CONSTRAINT_CONFLICT
     * (35) until DatSet itself was also cleared. Both together is what
     * actually releases it.
     *
     * Scoped to exactly the targets whose CURRENT live DatSet matches one of
     * ours (handle->domainScopedDynamicDatasetNames) - not blanket across
     * every target. Unlike a plain RptEna disable, clearing DatSet is
     * destructive to whatever that RCB was configured to report on, so this
     * must never touch an SCL-static target's permanent engineering
     * configuration, nor a foreign/adopted dataset another tool or client
     * still relies on - only a dataset THIS client itself created and is
     * about to try to delete below qualifies. Must run BEFORE the
     * dataset-delete loop below and BEFORE the caller's IedConnection_close
     * (both setRCBValues and deleteDataSet need a live association). */
    if (handle->connection && handle->domainScopedDynamicDatasetNames && handle->targets) {
        int trackedCount = LinkedList_size(handle->domainScopedDynamicDatasetNames);
        int targetCount = LinkedList_size(handle->targets);
        int matchedCount = 0;
        int unbindFailCount = 0;
        fprintf(stderr, "[mms_dataset_manager] stop: disabling+unbinding before delete - %d domain-scoped "
                "dataset(s) tracked, checking %d target(s) for a live match\n", trackedCount, targetCount);
        LinkedList element = LinkedList_getNext(handle->targets);
        while (element) {
            ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);
            IedClientError err = IED_ERROR_OK;
            ClientReportControlBlock rcb =
                    IedConnection_getRCBValues(handle->connection, &err, target->objectReference, NULL);
            if (rcb) {
                const char* liveDataSetLive = ClientReportControlBlock_getDataSetReference(rcb);
                /* Snapshot into an owned copy before mutating rcb below -
                 * ClientReportControlBlock_getDataSetReference returns a
                 * pointer into rcb's own internal buffer, which
                 * setDataSetReference("") overwrites in place; logging after
                 * that point off the original pointer would print the
                 * already-cleared value instead of what was actually there. */
                char* liveDataSet = liveDataSetLive ? MmsDatasetManagerUtils_safeStringDup(liveDataSetLive) : NULL;
                if (liveDataSet
                        && MmsDatasetManagerNaming_stringListContains(handle->domainScopedDynamicDatasetNames,
                                liveDataSet)) {
                    matchedCount++;
                    ClientReportControlBlock_setRptEna(rcb, false);
                    ClientReportControlBlock_setDataSetReference(rcb, "");
                    IedConnection_setRCBValues(handle->connection, &err, rcb,
                            RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_DATSET, true);
                    if (err != IED_ERROR_OK) {
                        unbindFailCount++;
                        fprintf(stderr, "[mms_dataset_manager] could not disable/unbind '%s' (dataset '%s') "
                                "before dataset cleanup: %s (%d)\n", target->objectReference, liveDataSet,
                                IedClientError_toString(err), err);
                    } else {
                        fprintf(stderr, "[mms_dataset_manager] disabled/unbound '%s' from dataset '%s'\n",
                                target->objectReference, liveDataSet);
                    }
                }
                free(liveDataSet);
                ClientReportControlBlock_destroy(rcb);
            }
            element = LinkedList_getNext(element);
        }
        fprintf(stderr, "[mms_dataset_manager] stop: disable+unbind pass done - %d target(s) matched a "
                "tracked dataset, %d succeeded, %d failed\n", matchedCount, matchedCount - unbindFailCount,
                unbindFailCount);
    }

    /* Self-created domain/VMD-scoped datasets, unlike the association-scoped
     * ones an unbuffered RCB normally gets, are NOT cleaned up automatically
     * by the server when the connection closes - this is the only place that
     * ever explicitly deletes them, and it must run BEFORE the caller's
     * IedConnection_close (deleteDataSet needs a live association). Without
     * this, every start/stop cycle against the same device leaks one more
     * dataset into its own total dataset-count budget until nothing is left
     * for anything else. Best-effort: a delete failure (already gone, device
     * rejects delete, connection already lost) just leaves a server-side
     * dataset behind - logged, not fatal to stopping. */
    if (handle->connection && handle->domainScopedDynamicDatasetNames) {
        int toDelete = LinkedList_size(handle->domainScopedDynamicDatasetNames);
        int deletedOnStop = 0;
        int leakedOnStop = 0;
        fprintf(stderr, "[mms_dataset_manager] stop: attempting to delete %d domain-scoped dataset(s)\n", toDelete);
        LinkedList element = LinkedList_getNext(handle->domainScopedDynamicDatasetNames);
        while (element) {
            char* datasetName = (char*) LinkedList_getData(element);
            IedClientError deleteErr = IED_ERROR_OK;
            if (!IedConnection_deleteDataSet(handle->connection, &deleteErr, datasetName)) {
                fprintf(stderr, "[mms_dataset_manager] could not delete dynamic dataset '%s' on stop: "
                        "%s (%d) - left behind on the device\n", datasetName,
                        IedClientError_toString(deleteErr), deleteErr);
                leakedOnStop++;
            } else {
                fprintf(stderr, "[mms_dataset_manager] deleted dynamic dataset '%s' on stop\n", datasetName);
                deletedOnStop++;
            }
            element = LinkedList_getNext(element);
        }
        fprintf(stderr, "[mms_dataset_manager] stop: dataset deletion done - %d of %d succeeded, %d left "
                "behind\n", deletedOnStop, toDelete, leakedOnStop);
        if (leakedOnStop > 0) {
            fprintf(stderr, "[mms_dataset_manager] %d domain-scoped dataset(s) could not be deleted on stop - "
                    "permanently spent against this device's dataset quota until a device-side reset\n",
                    leakedOnStop);
        }
        LinkedList_destroyDeep(handle->domainScopedDynamicDatasetNames, free);
        handle->domainScopedDynamicDatasetNames = NULL;
    }
}
