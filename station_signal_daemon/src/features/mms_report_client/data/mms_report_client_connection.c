#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_report_client/data/mms_report_client_connection.h"
#include "features/mms_report_client/data/mms_report_client_report_adapter.h"
#include "features/mms_report_client/data/mms_report_client_auth.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"
#include "hal_time.h"

/* A connection must stay up at least this long before a subsequent loss
 * resets the exponential backoff back to the initial tier - see
 * supervisorLoop's own comment on why. */
#define MMS_REPORT_CLIENT_STABLE_CONNECTION_MS 5000

/*
 * Fires while an internal state mutex is held (iec61850_client.h) - must not
 * call IedConnection_getState or any blocking IedConnection_* function from
 * here. Only ever sets plain flags and posts the semaphore; the supervisor
 * thread does all the real work.
 */
static void
onStateChanged(void* parameter, IedConnection connection, IedConnectionState newState) {
    (void) connection;
    MmsReportClientHandle handle = (MmsReportClientHandle) parameter;

    if (newState == IED_STATE_CLOSED && !handle->stopRequested) {
        handle->connectionLostSignal = true;
    }

    if (handle->connStateCallback) {
        MmsReportClientConnState mapped;
        switch (newState) {
            case IED_STATE_CONNECTING: mapped = MMS_REPORT_CLIENT_CONNECTING; break;
            case IED_STATE_CONNECTED:  mapped = MMS_REPORT_CLIENT_CONNECTED; break;
            default:                   mapped = MMS_REPORT_CLIENT_DISCONNECTED; break;
        }
        handle->connStateCallback(handle->connStateCallbackParam, mapped);
    }

    Semaphore_post(handle->wakeSignal);
}

/* One entry per Dyn RCB target that was assigned a dynamically-created
 * dataset within the current connect cycle - see getOrCreateDynamicDataset's
 * own doc comment. Built fresh in enableAllTargets for every (re)connect and
 * discarded at the end of that same call; never carried across reconnects
 * (the @-scoped datasets it names don't survive a reconnect either - see
 * MmsReportClientConnection_create's own comment on the connection object's
 * own reuse for the parallel reasoning). */
typedef struct {
    char* lnReference;  /* owned copy, matches ReportControlBlockTarget.objectReference -
                            every Dyn target gets its own uniquely-clustered dataset now,
                            keyed by its own objectReference, never shared with another
                            target's own assignment (field name kept for minimal diff
                            against this struct's original single-LN-dedup design). */
    char* datasetName;  /* owned copy, e.g. "@dyn_E13_6MD_PTOC1" (unbuffered) or
                            "E13_6MD/PTOC1$dyn" (buffered) */
    bool buffered;      /* part of the lookup key alongside lnReference - technically
                            redundant now that every key is already a unique
                            objectReference, kept for the naming-scheme distinction
                            buildDynamicDatasetName still needs (buffered vs
                            association-scoped). */
} DynamicDatasetCacheEntry;

static void
destroyDynamicDatasetCacheEntry(void* entry) {
    if (!entry) return;
    DynamicDatasetCacheEntry* e = (DynamicDatasetCacheEntry*) entry;
    free(e->lnReference);
    free(e->datasetName);
    free(e);
}

static const char*
lookupDynamicDatasetName(LinkedList cache, const char* lnReference, bool buffered) {
    if (!cache || !lnReference) return NULL;

    LinkedList element = LinkedList_getNext(cache);
    while (element) {
        DynamicDatasetCacheEntry* entry = (DynamicDatasetCacheEntry*) LinkedList_getData(element);
        if (entry->lnReference && entry->buffered == buffered
                && strcmp(entry->lnReference, lnReference) == 0) {
            return entry->datasetName;
        }
        element = LinkedList_getNext(element);
    }
    return NULL;
}

/*
 * Everything getOrCreateDynamicDataset/adoptUnclaimedDataset need for one
 * connect cycle: the existing LN-keyed dedup cache, plus a budget counter
 * seeded from SCL's own <Services><DynDataSet max="N"/> (IedModel_getDynDataSetMax)
 * CORRECTED for datasets already discovered on the server
 * (MmsReportClientUseCases_computeInitialDynamicDatasetBudget) at the top of
 * enableAllTargets - not just a blind copy of the declared max, which has no
 * awareness of what's already consuming the device's real budget (leftover
 * domain-scoped datasets from an earlier ungracefully-terminated run, other
 * clients'/tools' own datasets, etc. - a real device run showed exactly this
 * silently exhausting the real budget while this client's own naive counter
 * still believed most of it remained). remainingBudget models "how many MORE
 * of our own new createDataSet attempts this connect cycle may still make".
 * -1 (SCL never declared a cap) must never trigger the short-circuit - see
 * MmsReportClientUseCases_isDynamicDatasetBudgetExhausted. Decremented only
 * on a genuinely new successful createDataSet, never on a cache hit or an
 * adopted existing dataset (both are free). budgetExhaustedLogged
 * edge-triggers the "stopping" log so it fires once per cycle, not once per
 * remaining target - without this, a device with many RCBs past the budget
 * wall would print one near-identical line per remaining target instead of
 * one.
 *
 * Built fresh in enableAllTargets for every (re)connect, same lifetime as
 * DynamicDatasetCacheEntry's own cache - see that struct's doc comment for why
 * neither is carried across reconnects (existingServerDatasets/claimedDatasetNames
 * share the same reasoning: server-side state may have genuinely changed
 * since the last connect, so discovery re-runs fresh every time too).
 */
typedef struct {
    LinkedList cache;
    int remainingBudget;
    bool budgetExhaustedLogged;
    LinkedList chunkAssignments; /* DynamicDatasetChunkAssignment* list - see that struct's own doc
                                     comment and buildWholeDeviceClusterPlan. Empty (never NULL)
                                     only if the device has no Dyn RCB slots at all or no
                                     reportable data anywhere in the model. */
    LinkedList existingServerDatasets; /* owned char* list - see discoverExistingServerDatasets'
                                     own doc comment. Every dataset already on the server across
                                     this client's own Dyn targets' LDs, discovered once per
                                     connect cycle. */
    LinkedList claimedDatasetNames; /* owned char* list - see adoptUnclaimedDataset's own doc
                                     comment. Tracks which existingServerDatasets entries have
                                     already been tried/claimed this cycle, whether successfully
                                     adopted or found unusable. */
} DynamicDatasetSession;

/*
 * One whole-device cluster (a DO-atomic group of reportable leaves, possibly
 * spanning several different LNs - see buildWholeDeviceClusterPlan's own doc
 * comment for how/when this gets populated and by which of its two
 * strategies) assigned to ONE Dyn RCB slot anywhere on the device - 1:1,
 * never shared. rcbReference matches that target's own objectReference (not
 * lnReference - every assigned target gets its own unique dataset, since a
 * cluster's content generally has nothing to do with the target's own parent
 * LN anymore).
 */
typedef struct {
    char* rcbReference;      /* owned copy, == the assigned target's own objectReference */
    char** memberReferences; /* owned array of owned strings - this chunk's own DO-atomic subset */
    int memberCount;
} DynamicDatasetChunkAssignment;

static void
destroyDynamicDatasetChunkAssignment(void* entry) {
    if (!entry) return;
    DynamicDatasetChunkAssignment* e = (DynamicDatasetChunkAssignment*) entry;
    free(e->rcbReference);
    if (e->memberReferences) {
        for (int i = 0; i < e->memberCount; i++) free(e->memberReferences[i]);
        free(e->memberReferences);
    }
    free(e);
}

static DynamicDatasetChunkAssignment*
lookupChunkAssignment(LinkedList chunkAssignments, const char* rcbReference) {
    if (!chunkAssignments || !rcbReference) return NULL;

    LinkedList element = LinkedList_getNext(chunkAssignments);
    while (element) {
        DynamicDatasetChunkAssignment* entry = (DynamicDatasetChunkAssignment*) LinkedList_getData(element);
        if (entry->rcbReference && strcmp(entry->rcbReference, rcbReference) == 0) return entry;
        element = LinkedList_getNext(element);
    }
    return NULL;
}

/* Small duplicated lookup (mirrors mms_report_client_report_adapter.c's own
 * lookupMemberRefCache) - both are tiny, data-layer-local, and this feature's
 * own convention already duplicates snippets this size across files rather
 * than sharing them (see e.g. the ACSE-auth-setup duplication between this
 * feature and scl_bootstrap). */
static MmsReportClientMemberRefCacheEntry*
lookupMemberRefCacheByRcb(MmsReportClientHandle handle, const char* rcbReference) {
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

/* Two distinct naming schemes depending on `buffered`:
 *
 *   - UNBUFFERED (the pre-existing scheme, unchanged): "@"-prefixed =>
 *     association-scoped (destroyed automatically when this connection
 *     closes - see IedConnection_createDataSet's own doc comment) - no
 *     explicit delete needed, no risk of leaking the device's dataset budget
 *     across reconnects/restarts. lnReference is sanitized ('/' -> '_')
 *     purely so the generated name reads sensibly in logs; createDataSet
 *     doesn't require any particular naming beyond the leading "@".
 *
 *   - BUFFERED: an association-scoped dataset is destroyed the instant this
 *     connection closes - semantically incompatible with a buffered RCB,
 *     whose whole purpose is to keep reporting through a disconnect.
 *     Confirmed directly against the vendored reference server
 *     (mms_mapping/reporting.c's updateReportDataset: an "@"-prefixed
 *     dataSetName is rejected outright when rc->buffered is true, surfacing
 *     to the client as IED_ERROR_OBJECT_VALUE_INVALID/32) and against a real
 *     SIPROTEC 6MD device (see GAP3_DYNAMIC_DATASET_NOTES.md). Instead builds
 *     a domain/VMD-scoped name - "$"-joined, no "@" prefix, same convention
 *     ied_model already uses for an SCL-declared datasetReference
 *     (IedModelUseCases_getReportSubscriptionTargets) - which persists on the
 *     server past this connection, exactly what a buffered RCB needs.
 *     lnReference already has the required "LDName/LNodeName" shape
 *     (IedConnection_createDataSet's own doc comment: "LDName/LNodeName.dataSetName
 *     for permanent domain or VMD scope data sets" - the client library
 *     splits on the first '/' for the domain, then converts any '.' in the
 *     remainder to '$' for the wire form). Whole-device clustering means the
 *     input here is often target->objectReference instead (e.g.
 *     "LDName/LNName.BR.rcbName", so each Dyn target gets its own uniquely
 *     named dataset rather than sharing an LN-wide one) - THAT string
 *     contains a literal '.' (the ".BR."/".RP." RCB-instance segment), which
 *     createDataSet's own internal conversion would silently fold to '$' on
 *     the wire, but this function's own returned string would NOT reflect
 *     unless it applies the identical conversion itself: any '.' is
 *     explicitly replaced with '$' below before appending "$dyn", so the
 *     name we keep for ClientReportControlBlock_setDataSetReference always
 *     matches the real server-side name bit-for-bit, regardless of createDataSet's
 *     own internal behavior. Confirmed as a real, previously-latent bug: a
 *     dot left unconverted here produces a dataset reference the server
 *     genuinely can't resolve, surfacing as IED_ERROR_OBJECT_VALUE_INVALID/32
 *     on the following setRCBValues - identical-looking symptom to, but a
 *     completely different root cause from, the association-scoped-on-a-
 *     buffered-RCB bug this whole function exists to fix.
 *     Deterministic and stable across reconnects/restarts by construction -
 *     required so a later connect recognizes and reuses its own
 *     already-created dataset (see createAndCacheDynamicDataset's
 *     IED_ERROR_OBJECT_EXISTS handling) instead of erroring or duplicating,
 *     and so MmsReportClientConnection_stop's explicit cleanup can find it
 *     again by name. */
static char*
buildDynamicDatasetName(const char* lnReference, bool buffered) {
    if (buffered) {
        size_t len = strlen(lnReference) + strlen("$dyn") + 1;
        char* name = malloc(len);
        if (!name) return NULL;
        snprintf(name, len, "%s$dyn", lnReference);
        for (char* p = name; *p; p++) {
            if (*p == '.') *p = '$';
        }
        return name;
    }

    size_t len = strlen("@dyn_") + strlen(lnReference) + 1;
    char* name = malloc(len);
    if (!name) return NULL;
    snprintf(name, len, "@dyn_%s", lnReference);
    for (char* p = name; *p; p++) {
        if (*p == '/') *p = '_';
    }
    return name;
}

/* Dedup-inserting append to handle->domainScopedDynamicDatasetNames (see that
 * field's own doc comment) - called only for buffered/domain-scoped names,
 * never for "@"-scoped ones (those need no cleanup tracking at all). Cheap
 * linear scan: this list holds at most one entry per LN needing a buffered
 * self-created dataset, realistically single digits to tens of entries even
 * on a large IED. */
static void
rememberDomainScopedDatasetName(MmsReportClientHandle handle, const char* datasetName) {
    if (!handle->domainScopedDynamicDatasetNames) {
        handle->domainScopedDynamicDatasetNames = LinkedList_create();
        if (!handle->domainScopedDynamicDatasetNames) return;
    }

    LinkedList element = LinkedList_getNext(handle->domainScopedDynamicDatasetNames);
    while (element) {
        if (strcmp((char*) LinkedList_getData(element), datasetName) == 0) return;
        element = LinkedList_getNext(element);
    }

    LinkedList_add(handle->domainScopedDynamicDatasetNames, MmsReportClientUtils_safeStringDup(datasetName));
}

/*
 * Shared createDataSet + cache-insert + budget-decrement plumbing for
 * getOrCreateDynamicDataset: `cacheKey` is always target->objectReference
 * (already unique per target, doubling as the naming/cache key with no
 * separate scheme needed) and `memberReferences`/`memberCount` are this
 * target's own whole-device cluster assignment. `logLnReference`/
 * `logRcbReference` are for messages only. Caller has already confirmed the
 * budget isn't exhausted.
 */
static const char*
createAndCacheDynamicDataset(MmsReportClientHandle handle, DynamicDatasetSession* session, const char* cacheKey,
        const char* logLnReference, const char* logRcbReference, const char* const* memberReferences,
        int memberCount, bool buffered) {
    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(memberReferences, memberCount);
    if (!wireRefs || LinkedList_size(wireRefs) == 0) {
        fprintf(stderr, "[mms_report_client] no wire-convertible attribute references for LN '%s' - "
                "'%s' will not get a dynamic dataset\n", logLnReference, logRcbReference);
        if (wireRefs) LinkedList_destroyDeep(wireRefs, free);
        return NULL;
    }

    char* datasetName = buildDynamicDatasetName(cacheKey, buffered);
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
    fprintf(stderr, "[mms_report_client] creating dynamic dataset '%s' for LN '%s' with %d member attribute(s)\n",
            datasetName, logLnReference, memberCount);

    IedClientError err = IED_ERROR_OK;
    IedConnection_createDataSet(handle->connection, &err, datasetName, wireRefs);
    LinkedList_destroyDeep(wireRefs, free);

    /* Domain-scoped names are deterministic and persist past a connection
     * (unlike the "@"-scoped ones), so a reconnect - or a prior daemon run
     * that never got to clean up - can legitimately find this exact name
     * already on the server. Treat that as a successful reuse, not a
     * failure: the member list is derived the same deterministic way every
     * time (every FC=ST/MX leaf under this LN, or this target's own chunk),
     * so an existing dataset by this name is presumed to already have the
     * right shape. */
    bool reusedExisting = false;
    if (err == IED_ERROR_OBJECT_EXISTS && buffered) {
        fprintf(stderr, "[mms_report_client] dynamic dataset '%s' for LN '%s' already exists on the server - "
                "reusing it\n", datasetName, logLnReference);
        reusedExisting = true;
    } else if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] dynamic dataset creation failed for LN '%s': error %d - "
                "'%s' will not report\n", logLnReference, err, logRcbReference);
        free(datasetName);
        return NULL;
    }

    if (!reusedExisting) {
        fprintf(stderr, "[mms_report_client] created dynamic dataset '%s' for LN '%s'\n", datasetName, logLnReference);
        /* Only a genuinely NEW dataset counts against this cycle's budget -
         * a reused pre-existing one was never new against the device's own
         * count this cycle (see DynamicDatasetSession's own doc comment on
         * why cache hits stay free; this is the same reasoning applied to a
         * dataset the SERVER already knew about before this cycle started). */
        if (session->remainingBudget > 0) session->remainingBudget--;
    }

    if (buffered) rememberDomainScopedDatasetName(handle, datasetName);

    DynamicDatasetCacheEntry* cacheNode = malloc(sizeof(DynamicDatasetCacheEntry));
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
    cacheNode->lnReference = MmsReportClientUtils_safeStringDup(cacheKey);
    cacheNode->datasetName = datasetName;
    cacheNode->buffered = buffered;
    LinkedList_add(session->cache, cacheNode);

    return cacheNode->datasetName;
}

/*
 * For an RCB whose SCL declared no datSet (datasetReference == NULL,
 * datSet="Dyn" in SCL terms - see CLAUDE.md's own bullet on this): resolves
 * (creating if needed) the dataset buildWholeDeviceClusterPlan assigned this
 * target this cycle (session->chunkAssignments) - a cluster of reportable
 * leaves that may span several different LNs across the WHOLE device, not
 * just this target's own parent LN (see that function's own doc comment for
 * why a Dyn RCB's parent LN doesn't constrain what it can report on). Always
 * keyed/named by target->objectReference - no LN-wide dedup/sharing (unlike
 * this feature's original single-LN design, every Dyn target now gets its
 * own uniquely-clustered dataset, since two different targets' clusters
 * generally have nothing in common).
 *
 * Returns the dataset name (borrowed - owned by session->cache, valid for
 * the rest of the current connect cycle) on success, or NULL if this target
 * received no cluster assignment this cycle (the device's reportable data
 * was already fully covered by other slots - see buildWholeDeviceClusterPlan's
 * own overflow logging), if this connect cycle's own dataset-count budget is
 * already exhausted (see DynamicDatasetSession's own doc comment), or if
 * dataset creation itself failed (cap exceeded, maxAttributes exceeded,
 * etc.) - either way, the caller falls back to today's pre-existing behavior
 * (skip DATSET, let setRCBValues fail, log, move on).
 */
static const char*
getOrCreateDynamicDataset(MmsReportClientHandle handle, ReportControlBlockTarget* target,
        DynamicDatasetSession* session) {
    if (!session || !target->lnReference) return NULL;

    DynamicDatasetChunkAssignment* chunk =
            lookupChunkAssignment(session->chunkAssignments, target->objectReference);
    if (!chunk) return NULL;

    const char* existing = lookupDynamicDatasetName(session->cache, target->objectReference, target->buffered);
    if (existing) return existing;

    /* Cache hits above stay free (zero wire cost, always allowed) - only a
     * genuinely NEW createDataSet attempt is gated by the budget. */
    if (MmsReportClientUseCases_isDynamicDatasetBudgetExhausted(session->remainingBudget)) {
        if (!session->budgetExhaustedLogged) {
            fprintf(stderr, "[mms_report_client] dynamic dataset count budget (SCL DynDataSet max=%d) "
                    "exhausted this connect cycle - no further createDataSet attempts will be made; "
                    "remaining RCB(s) needing a dynamic dataset will not report\n",
                    IedModel_getDynDataSetMax(handle->iedModel));
            session->budgetExhaustedLogged = true;
        }
        return NULL;
    }

    return createAndCacheDynamicDataset(handle, session, target->objectReference, target->lnReference,
            target->objectReference, (const char* const*) chunk->memberReferences, chunk->memberCount,
            target->buffered);
}

/* Cheap, purely-local skip: true if datasetRef looks like a name
 * buildDynamicDatasetName would itself generate for this LN - i.e. a
 * DANGLING reference to a PRIOR connection's own association-scoped dataset,
 * already destroyed server-side the moment that old connection closed (see
 * buildDynamicDatasetName's own doc comment: "@"-prefixed = destroyed
 * automatically on connection close). A real device is not obligated to
 * clear an RCB's DatSet attribute just because the dataset object it named
 * is gone, so getRCBValues can legitimately still echo it back after we
 * reconnect. This is belt-and-suspenders only, not the sole safety net - the
 * real one is pullLiveDataset's own IedConnection_getDataSetDirectory failure
 * handling below, which falls through to getOrCreateDynamicDataset either
 * way (a dangling reference would fail to resolve there too).
 *
 * Checks the one naming scheme getOrCreateDynamicDataset can produce for
 * this target: buildDynamicDatasetName(objectReference, target->buffered) -
 * every Dyn target is keyed/named by its own objectReference now (whole-
 * device clustering assigns each target its own unique cluster, no more
 * LN-wide shared name). target->buffered is a fixed, model-level property of
 * this specific RCB, so whichever scheme (association-scoped vs
 * domain-scoped) this target would itself produce is the only one relevant
 * here. Checked unconditionally, not gated on whether this target has a
 * cluster assignment THIS cycle - it may have been assigned one on a prior
 * cycle whose dataset (association-scoped and gone, or a domain-scoped one
 * this same client already owns and would otherwise re-detect as "live") is
 * now dangling/stale from this cycle's point of view. */
static bool
looksLikeOurOwnDynamicDatasetName(const char* datasetRef, const ReportControlBlockTarget* target) {
    if (!datasetRef || !target) return false;

    if (target->objectReference) {
        char* expectedChunked = buildDynamicDatasetName(target->objectReference, target->buffered);
        bool match = expectedChunked && strcmp(datasetRef, expectedChunked) == 0;
        free(expectedChunked);
        if (match) return true;
    }

    return false;
}

/*
 * Tier 2 of the static -> pull live -> self-create dataset resolution order
 * (see enableOneTarget's own doc comment): for an RCB with no SCL datSet, a
 * dataset may already be assigned to it on the live device right now -
 * realistically, by a commissioning/engineering tool (e.g. Siemens DIGSI)
 * during substation engineering, independent of whatever client connects
 * later; whoever set it and whether they're still associated is irrelevant,
 * the live RCB value is the only signal consulted.
 * ClientReportControlBlock_getDataSetReference(rcb) is free here - `rcb` is
 * already fetched by enableOneTarget's own IedConnection_getRCBValues call,
 * no extra wire round-trip for the read itself.
 *
 * Unlike the static (SCL) case, this dataset's real member list is NOT known
 * locally - IedModel never had a matching DataSet registered for this RCB
 * (SCL declared no datSet at all). A single, bounded
 * IedConnection_getDataSetDirectory call resolves the actual member list,
 * live - narrower than the "no over-the-wire tree discovery" Hard Rule's
 * existing ied_model_online_loader exception on every axis: one already-named
 * dataset's member list (not a tree walk), fired only for RCBs with no SCL
 * datSet (the same population getOrCreateDynamicDataset already makes a wire
 * call for today via IedConnection_createDataSet - a peer call on the same
 * RCB set, not a new category gaining wire access), and at most once per
 * (RCB, live-dataset-identity) pair for the whole process lifetime thanks to
 * MmsReportClientMemberRefCacheEntry.resolvedDatasetReference's own
 * fingerprint check in refreshPulledMemberRefCache below.
 *
 * Returns true and fills *outMemberRefs (LinkedList of owned "$"-joined,
 * LD-prefixed member-reference char*, this feature's own memberReferences[]
 * convention) on success. Returns false (*outMemberRefs left untouched) if no
 * DatSet is currently assigned, it looks like our own dangling name (see
 * looksLikeOurOwnDynamicDatasetName), or the live fetch itself fails or
 * yields zero wire-convertible members - the caller falls through to
 * getOrCreateDynamicDataset unchanged in every one of these cases, exactly as
 * if this tier didn't exist.
 */
static bool
pullLiveDataset(MmsReportClientHandle handle, ReportControlBlockTarget* target, ClientReportControlBlock rcb,
        LinkedList* outMemberRefs) {
    const char* liveDataset = ClientReportControlBlock_getDataSetReference(rcb);
    if (!liveDataset || liveDataset[0] == '\0') return false;
    if (looksLikeOurOwnDynamicDatasetName(liveDataset, target)) return false;

    IedClientError err = IED_ERROR_OK;
    bool isDeletable = false;
    LinkedList acsiMembers = IedConnection_getDataSetDirectory(handle->connection, &err, liveDataset, &isDeletable);
    if (!acsiMembers || LinkedList_size(acsiMembers) == 0) {
        fprintf(stderr, "[mms_report_client] live-assigned dataset '%s' for '%s' could not be resolved "
                "(error %d) - will create our own instead\n", liveDataset, target->objectReference, err);
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
        char* memberRef = MmsReportClientUseCases_convertAcsiRefToMemberReference(acsiRef);
        if (memberRef) {
            LinkedList_add(wireRefs, memberRef);
        } else {
            fprintf(stderr, "[mms_report_client] could not convert live dataset member '%s' "
                    "(dataset '%s') to wire form - member skipped\n", acsiRef, liveDataset);
        }
        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(acsiMembers, free);

    if (LinkedList_size(wireRefs) == 0) {
        fprintf(stderr, "[mms_report_client] live-assigned dataset '%s' for '%s' had no wire-convertible "
                "members - will create our own instead\n", liveDataset, target->objectReference);
        LinkedList_destroyDeep(wireRefs, free);
        return false;
    }

    fprintf(stderr, "[mms_report_client] reusing live-assigned dataset '%s' for '%s' (%d member(s))\n",
            liveDataset, target->objectReference, LinkedList_size(wireRefs));

    *outMemberRefs = wireRefs;
    return true;
}

/*
 * Rebuilds (or confirms up to date) this RCB's memberRefCache entry to match
 * `liveDataset`'s real shape - called from enableOneTarget on every
 * (re)connect where pullLiveDataset (tier 2) succeeded. Compares liveDataset
 * against the cache entry's own resolvedDatasetReference fingerprint
 * (MmsReportClientMemberRefCacheEntry's own doc comment): identical string
 * means this entry's shape (built on a PRIOR connect from this exact dataset)
 * is still correct - a no-op, the expected common case on every reconnect
 * once a stable commissioning-tool-assigned dataset has been pulled once.
 * Different (including NULL - i.e. never resolved from a live connection
 * before, this RCB's very first successful pull) means the previously-cached
 * shape can no longer be trusted: rebuilds a fresh
 * MmsReportClientMemberRefCacheEntry from memberRefs via
 * MmsReportClientUseCases_buildMemberRefCacheEntry, then swaps every
 * array-shaped field into the EXISTING entry in place, under
 * memberRefCacheLock, via MmsReportClientUseCases_swapMemberRefCacheEntryShape
 * - never reallocates or moves the entry's own pointer, or touches
 * handle->memberRefCache's list structure itself, since the report-adapter
 * thread's own lookupMemberRefCache walks that list WITHOUT holding the lock
 * (only field mutations are lock-protected - see that function's own doc
 * comment in mms_report_client_report_adapter.c).
 *
 * This is a DELIBERATE, NARROW exception to "the value-diff cache is never
 * reset" (MmsReportClientMemberRefCacheEntry's own top doc comment): a shape
 * rebuild resets lastForwardedValues/leafSlotOffsets/everPopulated/lastEntryId
 * back to a fresh bootstrap state too (see
 * MmsReportClientUseCases_swapMemberRefCacheEntryShape), because a shape
 * change invalidates slot INDICES themselves - slot 3 under the old shape and
 * slot 3 under the new shape are not the same wire position, so preserving
 * old slot contents across a shape change would silently misattribute a
 * stale value to an unrelated new position. In practice this fires at most
 * once per RCB (first successful pull) unless the live-assigned dataset's
 * identity genuinely changes underneath the daemon mid-deployment.
 *
 * No-op if this RCB has no existing cache entry at all (a target with zero
 * dataset members never got one - see buildMemberRefCache's own "count > 0"
 * gate) or if memberRefs is empty.
 */
static void
refreshPulledMemberRefCache(MmsReportClientHandle handle, ReportControlBlockTarget* target,
        const char* liveDataset, LinkedList memberRefs) {
    if (!liveDataset) return;

    MmsReportClientMemberRefCacheEntry* entry = lookupMemberRefCacheByRcb(handle, target->objectReference);
    if (!entry) return;

    Semaphore_wait(handle->memberRefCacheLock);
    bool needsRebuild = !entry->resolvedDatasetReference || strcmp(entry->resolvedDatasetReference, liveDataset) != 0;
    Semaphore_post(handle->memberRefCacheLock);
    if (!needsRebuild) return;

    int count = memberRefs ? LinkedList_size(memberRefs) : 0;
    if (count <= 0) return;

    char** array = calloc((size_t) count, sizeof(char*));
    if (!array) return;
    int i = 0;
    for (LinkedList el = LinkedList_getNext(memberRefs); el; el = LinkedList_getNext(el)) {
        array[i++] = MmsReportClientUtils_safeStringDup((char*) LinkedList_getData(el));
    }

    MmsReportClientMemberRefCacheEntry* fresh = MmsReportClientUseCases_buildMemberRefCacheEntry(
            handle->iedModel, target->objectReference, array, count, liveDataset);
    if (!fresh) return;

    Semaphore_wait(handle->memberRefCacheLock);
    MmsReportClientUseCases_swapMemberRefCacheEntryShape(entry, fresh);
    Semaphore_post(handle->memberRefCacheLock);
}

/* Small owned-string-list membership check - used by discoverExistingServerDatasets
 * to dedupe which LDs it has already queried, and by adoptUnclaimedDataset to
 * track which discovered dataset names have already been tried/claimed this
 * cycle (so the same unusable candidate isn't re-probed for every remaining
 * target, and the same usable one isn't handed to two different targets). */
static bool
stringListContains(LinkedList list, const char* value) {
    if (!list || !value) return false;
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        char* entry = (char*) LinkedList_getData(element);
        if (entry && strcmp(entry, value) == 0) return true;
        element = LinkedList_getNext(element);
    }
    return false;
}

/*
 * Discovers every dataset already sitting on the server, once per connect
 * cycle (called at the top of enableAllTargets, before any dataset is
 * created), scoped to the distinct set of LDs this client's own Dyn targets
 * live under. Feeds two things: (1) an accurate starting budget (see
 * MmsReportClientUseCases_computeInitialDynamicDatasetBudget) instead of
 * blindly trusting SCL's declared max with zero awareness of what already
 * exists (confirmed on a real device: a leftover pile of domain-scoped
 * datasets from an earlier ungracefully-terminated run silently ate most of
 * the real budget before this client's own counter believed anything was
 * wrong); (2) a pool adoptUnclaimedDataset draws from to reuse an existing
 * dataset instead of self-creating a new one - "primarily try to use
 * existing/foreign datasets and create our own only via necessity," per
 * explicit user direction.
 *
 * Uses IedConnection_getLogicalDeviceDataSets (third_party/include/iec61850_client.h)
 * - a real, always-fresh wire query returning every dataset name under one
 * LD, in bare MMS notation (e.g. "LLN0$dyn", NOT LD-prefixed) - prefixed with
 * "<ldName>/" here to produce the same full "LD/LN..." reference form this
 * feature uses everywhere else. Domain/VMD-scoped datasets only - an
 * association-scoped ("@"-prefixed) one belongs to whichever OTHER
 * connection created it and isn't stored under any LD's own namespace, so it
 * can never appear here and never needs to (it's already gone the moment
 * that other connection closes, never a discovery/reuse candidate).
 *
 * Scoped to Dyn targets' own LDs only, not a full-device sweep of every LD
 * regardless of relevance - keeps the added round-trip cost proportional to
 * what this client actually needs (typically far fewer LDs than the device's
 * total), at the honest cost of not catching a stale leftover under an LD
 * that no longer has any Dyn target at all (an LN/RCB removed since the
 * leftover was created) - a real but narrow gap, not worth a full-device
 * sweep's extra cost to close today.
 *
 * Returns a LinkedList of owned, full "LD/LN...$..." reference strings
 * (never NULL, empty if no Dyn targets or nothing discovered). Caller owns:
 * LinkedList_destroyDeep(list, free).
 */
static LinkedList
discoverExistingServerDatasets(MmsReportClientHandle handle) {
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

        if (stringListContains(processedLds, ldName)) {
            free(ldName);
            continue;
        }
        LinkedList_add(processedLds, ldName); /* processedLds now owns ldName */

        IedClientError err = IED_ERROR_OK;
        LinkedList mmsNames = IedConnection_getLogicalDeviceDataSets(handle->connection, &err, ldName);
        if (!mmsNames) continue;

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
        }
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
     * discoverExistingServerDatasets' own doc comment on scope). */
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
            LinkedList_remove(existing, candidate);
            free(candidate);
        }
    }

    if (LinkedList_size(existing) > 0) {
        fprintf(stderr, "[mms_report_client] discovered %d existing dataset(s) already on the server "
                "across this client's own LD(s)\n", LinkedList_size(existing));
    }

    return existing;
}

/*
 * Tier "adopt" of the dataset resolution order (see enableOneTarget's own
 * doc comment): before self-creating a NEW dataset for a Dyn target's own
 * whole-device cluster, check whether an existing, not-yet-claimed dataset
 * already sits on the server under this target's own LD (discoverExistingServerDatasets'
 * own pool, computed once per connect cycle) - "primarily try to use
 * existing/foreign datasets and create our own only via necessity," per
 * explicit user direction. Assigning an existing dataset to an RCB is
 * non-destructive and shareable - it doesn't modify the dataset object
 * itself, and any other client/tool that also references it (or created it)
 * is entirely unaffected - unlike deletion, which stays strictly limited to
 * datasets this client can prove it created itself (see the orphan-cleanup
 * pass), adoption applies to ANY existing dataset regardless of origin.
 *
 * Each adopted dataset is claimed by exactly ONE target this cycle (tracked
 * in session->claimedDatasetNames, checked before considering a candidate)
 * and never shared across multiple targets - maximizing distinct device
 * coverage the same way whole-device clustering's own fresh-create path
 * already does, rather than letting several targets redundantly adopt the
 * same one. A candidate found unusable (unresolvable or empty) is also
 * claimed, purely so it isn't re-probed via a wasted wire round-trip for
 * every remaining target this cycle.
 *
 * Mirrors pullLiveDataset's own contract closely (bool-shaped success +
 * outMemberRefs) so the caller (enableOneTarget) can reconcile decode shape
 * via the SAME refreshPulledMemberRefCache it already uses for tier 2 - an
 * adopted dataset's real content is resolved live, exactly like a
 * commissioning-tool-assigned one; the only difference is where the
 * reference itself came from (server-wide discovery vs. the RCB's own
 * already-set DatSet attribute). Unlike pullLiveDataset, the resolved name
 * isn't already known to the caller (there's no live RCB value to read it
 * from), so it's returned directly - borrowed, owned by
 * session->existingServerDatasets, valid for the rest of the current
 * connect cycle.
 *
 * Returns NULL (outMemberRefs left untouched) if this target's LN has no
 * '/' (malformed), no candidate exists under this target's own LD, or every
 * candidate found there is already claimed/unusable - the caller falls
 * through to getOrCreateDynamicDataset unchanged in every one of these
 * cases, exactly as if this tier didn't exist.
 */
static const char*
adoptUnclaimedDataset(MmsReportClientHandle handle, ReportControlBlockTarget* target, DynamicDatasetSession* session,
        LinkedList* outMemberRefs) {
    if (!session || !session->existingServerDatasets || !target->lnReference) return NULL;

    char* slash = strchr(target->lnReference, '/');
    if (!slash) return NULL;
    size_t ldLen = (size_t) (slash - target->lnReference);

    LinkedList element = LinkedList_getNext(session->existingServerDatasets);
    while (element) {
        char* candidate = (char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);

        if (strncmp(candidate, target->lnReference, ldLen) != 0 || candidate[ldLen] != '/') continue;
        if (stringListContains(session->claimedDatasetNames, candidate)) continue;

        IedClientError err = IED_ERROR_OK;
        bool isDeletable = false;
        LinkedList acsiMembers = IedConnection_getDataSetDirectory(handle->connection, &err, candidate, &isDeletable);
        if (!acsiMembers || LinkedList_size(acsiMembers) == 0) {
            fprintf(stderr, "[mms_report_client] discovered dataset '%s' could not be resolved (error %d) - "
                    "skipping as an adoption candidate\n", candidate, err);
            if (acsiMembers) LinkedList_destroyDeep(acsiMembers, free);
            LinkedList_add(session->claimedDatasetNames, MmsReportClientUtils_safeStringDup(candidate));
            continue;
        }

        LinkedList wireRefs = LinkedList_create();
        if (!wireRefs) {
            LinkedList_destroyDeep(acsiMembers, free);
            return NULL;
        }

        LinkedList acsiElement = LinkedList_getNext(acsiMembers);
        while (acsiElement) {
            char* acsiRef = (char*) LinkedList_getData(acsiElement);
            char* memberRef = MmsReportClientUseCases_convertAcsiRefToMemberReference(acsiRef);
            if (memberRef) LinkedList_add(wireRefs, memberRef);
            acsiElement = LinkedList_getNext(acsiElement);
        }
        LinkedList_destroyDeep(acsiMembers, free);

        if (LinkedList_size(wireRefs) == 0) {
            fprintf(stderr, "[mms_report_client] discovered dataset '%s' had no wire-convertible members - "
                    "skipping as an adoption candidate\n", candidate);
            LinkedList_destroyDeep(wireRefs, free);
            LinkedList_add(session->claimedDatasetNames, MmsReportClientUtils_safeStringDup(candidate));
            continue;
        }

        fprintf(stderr, "[mms_report_client] adopting existing dataset '%s' for '%s' (%d member(s)) - "
                "reused instead of self-creating\n", candidate, target->objectReference, LinkedList_size(wireRefs));

        LinkedList_add(session->claimedDatasetNames, MmsReportClientUtils_safeStringDup(candidate));
        *outMemberRefs = wireRefs;
        return candidate;
    }

    return NULL;
}

/*
 * Tier 3's counterpart of refreshPulledMemberRefCache, called once
 * effectiveDatasetReference is known to be getOrCreateDynamicDataset's own
 * deterministic per-target name (or NULL, if even self-creation failed - a
 * no-op, nothing to reconcile). Because that name/shape is 100% deterministic
 * per target and was ALREADY built into this entry as the cluster-fallback
 * PROVISIONAL shape at MmsReportClient_start time (buildMemberRefCache,
 * mms_report_client_api.c - tagged with a NULL resolvedDatasetReference
 * precisely so this first call always confirms it), this is bookkeeping-only
 * in the overwhelmingly common case: first-ever successful tier-3 enable, or
 * any reconnect after that (the name is deterministic, so it matches every
 * time) just stamps the fingerprint under the lock - zero array rebuilding,
 * zero extra calls beyond createDataSet itself. The only case this actually
 * rebuilds the shape is a transition BACK from a previously-active tier-2
 * pulled dataset (resolvedDatasetReference held some prior live dataset's
 * name, now gone/unresolvable this cycle) - in that one case, the shape is
 * recomputed from this target's own cluster assignment
 * (session->chunkAssignments - see buildWholeDeviceClusterPlan's own doc
 * comment) and swapped in exactly like refreshPulledMemberRefCache's own
 * shape-swap, with the same value-diff-cache-reset consequence and the same
 * rationale.
 *
 * REQUIRED for correctness, not just an optimization: getOrCreateDynamicDataset
 * only ever returns non-NULL when this target has a real cluster assignment
 * (whole-device clustering has no "unchunked, whole-LN" fallback anymore - a
 * target with no assignment gets no dataset at all, see that function's own
 * doc comment) - the rebuilt shape here must come from that exact same
 * cluster's own member subset, or a correctly-sized, cluster-scoped dataset
 * would exist on the wire while this entry's decode-time shape silently
 * diverges, corrupting report decoding on this target's very first enable.
 */
static void
ensureLnFallbackMemberRefCache(MmsReportClientHandle handle, ReportControlBlockTarget* target,
        const char* effectiveDatasetReference, DynamicDatasetSession* session) {
    if (!effectiveDatasetReference || !session) return;

    MmsReportClientMemberRefCacheEntry* entry = lookupMemberRefCacheByRcb(handle, target->objectReference);
    if (!entry) return;

    Semaphore_wait(handle->memberRefCacheLock);
    bool needsRebuild = !entry->resolvedDatasetReference
            || strcmp(entry->resolvedDatasetReference, effectiveDatasetReference) != 0;
    if (!needsRebuild) {
        Semaphore_post(handle->memberRefCacheLock);
        return;
    }
    Semaphore_post(handle->memberRefCacheLock);

    DynamicDatasetChunkAssignment* chunk = lookupChunkAssignment(session->chunkAssignments, target->objectReference);
    if (!chunk) return;

    int count = chunk->memberCount;
    char** array = NULL;
    if (count > 0) {
        array = calloc((size_t) count, sizeof(char*));
        if (!array) return;
        for (int i = 0; i < count; i++) {
            array[i] = MmsReportClientUtils_safeStringDup(chunk->memberReferences[i]);
        }
    }

    if (count <= 0) {
        /* Nothing reportable (mirrors getOrCreateDynamicDataset's own
         * "no reportable attributes" log-and-skip posture) - still stamp the
         * fingerprint so this rebuild isn't retried on every single enable. */
        Semaphore_wait(handle->memberRefCacheLock);
        free(entry->resolvedDatasetReference);
        entry->resolvedDatasetReference = MmsReportClientUtils_safeStringDup(effectiveDatasetReference);
        Semaphore_post(handle->memberRefCacheLock);
        return;
    }

    MmsReportClientMemberRefCacheEntry* fresh = MmsReportClientUseCases_buildMemberRefCacheEntry(
            handle->iedModel, target->objectReference, array, count, effectiveDatasetReference);
    if (!fresh) return;

    Semaphore_wait(handle->memberRefCacheLock);
    MmsReportClientUseCases_swapMemberRefCacheEntryShape(entry, fresh);
    Semaphore_post(handle->memberRefCacheLock);
}

static void
enableOneTarget(MmsReportClientHandle handle, ReportControlBlockTarget* target, DynamicDatasetSession* session) {
    /* Defense-in-depth against enableAllTargets' own loop-top check below -
     * covers the narrow gap between that check and this call actually
     * landing, if a stop lands on the connection concurrently mid-loop. */
    if (handle->stopRequested) return;

    IedClientError err = IED_ERROR_OK;

    ClientReportControlBlock rcb =
        IedConnection_getRCBValues(handle->connection, &err, target->objectReference, NULL);
    if (!rcb) {
        fprintf(stderr, "[mms_report_client] getRCBValues failed for '%s': error %d\n",
                target->objectReference, err);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false, err);
        }
        return;
    }

    /* Install the handler before enabling, so no report can arrive unhandled
     * in the gap between the two. */
    IedConnection_installReportHandler(handle->connection, target->objectReference,
            ClientReportControlBlock_getRptId(rcb), MmsReportClientReportAdapter_onReport, handle);

    /* The value-diff cache is deliberately NEVER reset here anymore - on the
     * very first connect it's already all-NULL (fresh from buildMemberRefCache),
     * and on every reconnect after that, the whole point is to PRESERVE the
     * real last-known values so this enable's own GI/redelivered snapshot
     * diffs against them instead of against a wiped-clean cache - a genuine
     * change made while disconnected now correctly forwards with a real
     * previousValue, and an unchanged resend is correctly suppressed by the
     * ordinary diff check. See MmsReportClientMemberRefCacheEntry's own doc
     * comment in mms_report_client_types.h for the full design and
     * shouldForwardAndUpdateCache's own doc comment (mms_report_client_usecases.c)
     * for exactly how a persistently-NULL slot past the first report is
     * detected and logged as a bug. */
    uint32_t mask = RCB_ELEMENT_RPT_ENA;

    /* Proactively request OptFlds.EntryID for every buffered RCB, rather
     * than relying on however the device happens to already be configured -
     * a real device was found sending ZERO EntryID across every single
     * report (confirmed via a temporary diagnostic log: 2581/2581 received
     * reports had no EntryID at all), making the EntryID-resumption
     * mechanism below structurally impossible against it regardless of how
     * correct our own resumption logic is, since there is never anything to
     * cache and resume from. ORs RPT_OPT_ENTRY_ID into whatever OptFlds bits
     * the device already has configured (read via
     * ClientReportControlBlock_getOptFlds, which reflects the device's own
     * current config since `rcb` was just populated from a real
     * IedConnection_getRCBValues call above) - never clobbers the rest, same
     * minimal-footprint posture as everywhere else in this function. Only
     * writes it back (and only then adds RCB_ELEMENT_OPT_FLDS to the mask)
     * if the bit isn't already set, to avoid touching this attribute on
     * every single reconnect once the device has accepted it once - unlike
     * DatSet/RptEna, OptFlds isn't expected to reset itself across
     * associations. The alternative (a site-side SCL/engineering-tool config
     * change enabling entryID="true" on the device itself) is noted in
     * CLAUDE.md's own mms_report_client bullet - this client-side approach
     * was chosen instead so the daemon works against a device's default
     * configuration without requiring a site visit/reconfiguration first. */
    if (target->buffered) {
        int currentOptFlds = ClientReportControlBlock_getOptFlds(rcb);
        if (!(currentOptFlds & RPT_OPT_ENTRY_ID)) {
            ClientReportControlBlock_setOptFlds(rcb, currentOptFlds | RPT_OPT_ENTRY_ID);
            mask |= RCB_ELEMENT_OPT_FLDS;
        }
    }

    /* DatSet must be (re-)set explicitly on enable - relying on a
     * server-side default dataset (configured only via ReportControlBlock_create's
     * dataSetName at server build time) is fragile: libiec61850's own
     * reference client example (client_example_no_thread.c) always sets
     * RCB_ELEMENT_DATSET alongside RPT_ENA too, using the same "$"-joined
     * reference format ied_model already hands us in datasetReference.
     *
     * Four-tier resolution order for which dataset to assign:
     *   1. STATIC - target->datasetReference, if SCL declared one for this
     *      RCB. Never touches the network; this branch is skipped entirely
     *      below whenever it applies.
     *   2. PULL LIVE - if neither SCL nor this branch, this is a "dynamic"
     *      RCB (IEC 61850 permits an RCB to exist with no dataset until one
     *      is assigned at runtime, datSet="Dyn" in SCL terms). Before
     *      creating our own, check whether the live device ALREADY has a
     *      dataset assigned to this RCB right now - realistically, one
     *      created by a commissioning/engineering tool (e.g. Siemens DIGSI)
     *      during substation engineering, independent of whatever client
     *      connects later; whoever set it and whether they're still
     *      connected is irrelevant, only the live RCB value matters (see
     *      pullLiveDataset's own doc comment). Reusing it means every report
     *      against this RCB must decode against ITS real member list, not the
     *      LN-wide fallback shape buildMemberRefCache provisionally seeded at
     *      start time - refreshPulledMemberRefCache reconciles that.
     *   3. ADOPT UNCLAIMED - if nothing is assigned to THIS RCB specifically,
     *      check whether an existing, not-yet-claimed dataset (ours from a
     *      prior run, or a completely foreign one from another tool)
     *      already sits on the server under this RCB's own LD
     *      (adoptUnclaimedDataset, fed by discoverExistingServerDatasets'
     *      once-per-cycle discovery) - "primarily try to use existing/foreign
     *      datasets and create our own only via necessity," per explicit user
     *      direction. Same decode-shape reconciliation as tier 2
     *      (refreshPulledMemberRefCache), since an adopted dataset's real
     *      content is resolved live exactly like a pulled one.
     *   4. SELF-CREATE - only if all three above are absent/unusable,
     *      getOrCreateDynamicDataset resolves whichever whole-device cluster
     *      buildWholeDeviceClusterPlan assigned this target this cycle (a
     *      DO-atomic group of reportable leaves that may span several
     *      different LNs across the ENTIRE device, not just this RCB's own
     *      parent LN - a Dyn RCB's parent LN doesn't restrict what a dataset
     *      assigned to it can report on, see that function's own doc
     *      comment). A target with no cluster assignment this cycle (the
     *      device's data was already fully covered by other slots) gets no
     *      dataset at all here. Unbuffered: association-scoped so it needs no
     *      explicit cleanup - UNCHANGED from this feature's original
     *      dynamic-dataset-creation behavior.
     *      Buffered: domain/VMD-scoped instead (buildDynamicDatasetName) -
     *      an association-scoped dataset is destroyed the instant this
     *      connection closes, which a real device rejects assigning to a
     *      buffered RCB outright (IED_ERROR_OBJECT_VALUE_INVALID/32 -
     *      confirmed against both the vendored reference server and a real
     *      SIPROTEC 6MD device, see GAP3_DYNAMIC_DATASET_NOTES.md); a
     *      domain-scoped one persists past this connection, which is what a
     *      buffered RCB needs, at the cost of needing explicit lifecycle
     *      management - see handle->domainScopedDynamicDatasetNames' own doc
     *      comment (mms_report_client_types.h) for reuse-on-reconnect and
     *      cleanup-on-stop. This tier's own mechanism/lifetime is not
     *      otherwise touched by tiers 1-3 existing above it, only the
     *      priority order is new.
     * If even tier 4 fails (no reportable attributes, or the device rejects
     * creation - cap exceeded, etc.), DATSET is left unset and setRCBValues
     * below fails with IED_ERROR_OBJECT_VALUE_INVALID, same as before this
     * feature existed. */
    const char* effectiveDatasetReference = target->datasetReference;
    const char* datasetTier = effectiveDatasetReference ? "SCL" : NULL;
    if (!effectiveDatasetReference) {
        LinkedList pulledMemberRefs = NULL;
        if (pullLiveDataset(handle, target, rcb, &pulledMemberRefs)) {
            const char* liveDataset = ClientReportControlBlock_getDataSetReference(rcb);
            refreshPulledMemberRefCache(handle, target, liveDataset, pulledMemberRefs);
            LinkedList_destroyDeep(pulledMemberRefs, free);
            effectiveDatasetReference = liveDataset;
            datasetTier = "live";
        } else {
            LinkedList adoptedMemberRefs = NULL;
            const char* adopted = adoptUnclaimedDataset(handle, target, session, &adoptedMemberRefs);
            if (adopted) {
                refreshPulledMemberRefCache(handle, target, adopted, adoptedMemberRefs);
                LinkedList_destroyDeep(adoptedMemberRefs, free);
                effectiveDatasetReference = adopted;
                datasetTier = "adopted";
            } else {
                effectiveDatasetReference = getOrCreateDynamicDataset(handle, target, session);
                ensureLnFallbackMemberRefCache(handle, target, effectiveDatasetReference, session);
                datasetTier = "self-created";
            }
        }
    }
    /* Which of the three tiers (see this function's own doc comment just
     * above) actually resolved the dataset - or that none did, the same
     * "DATSET left unset, setRCBValues about to fail" case documented there.
     * Logged unconditionally (not just on failure) so a real run shows, per
     * RCB, exactly which path was taken without having to infer it from
     * whichever failure fprintf's did or didn't fire. */
    fprintf(stderr, "[mms_report_client] '%s' dataset resolved via %s: '%s'\n", target->objectReference,
            effectiveDatasetReference ? datasetTier : "none",
            effectiveDatasetReference ? effectiveDatasetReference : "(none)");
    if (effectiveDatasetReference) {
        ClientReportControlBlock_setDataSetReference(rcb, effectiveDatasetReference);
        mask |= RCB_ELEMENT_DATSET;
    }

    /* Resume a buffered RCB's delivery from the last EntryID this client
     * actually received, instead of re-requesting the server's entire
     * unacknowledged backlog on every RptEna transition - see
     * MmsReportClientMemberRefCacheEntry.lastEntryId's own doc comment for
     * why this matters (a redelivered multi-entry backlog defeats the
     * single-slot value-diff cache). RCB_ELEMENT_ENTRY_ID is only meaningful
     * for buffered RCBs (iec61850_client.h) - gated on target->buffered.
     * lastEntryId is written by the report-adapter thread, so this read goes
     * through the same memberRefCacheLock that guards it; ClientReportControlBlock_setEntryId
     * itself is a local, synchronous struct mutation (confirmed against
     * libiec61850's own source - it clones the value internally), not a
     * network call, so it's safe to make while holding the lock, unlike
     * IedConnection_setRCBValues below. On the very first-ever enable
     * (lastEntryId still NULL - nothing to resume from yet) this is a no-op,
     * same full-backlog behavior as before this change. Computed BEFORE the
     * GI decision just below - hasResumableEntryId is that decision's own
     * input (see MmsReportClientUseCases_shouldRequestGiOnEnable). */
    bool hasResumableEntryId = false;
    if (target->buffered) {
        MmsReportClientMemberRefCacheEntry* cacheEntry = lookupMemberRefCacheByRcb(handle, target->objectReference);
        if (cacheEntry) {
            Semaphore_wait(handle->memberRefCacheLock);
            if (cacheEntry->lastEntryId) {
                ClientReportControlBlock_setEntryId(rcb, cacheEntry->lastEntryId);
                mask |= RCB_ELEMENT_ENTRY_ID;
                hasResumableEntryId = true;
            }
            Semaphore_post(handle->memberRefCacheLock);
        }
    }

    ClientReportControlBlock_setRptEna(rcb, true);
    /* GI used to be requested on every enable unconditionally. Real-hardware
     * logs showed a buffered RCB's own GI response getting enqueued into its
     * buffered backlog as a brand-new entry (fresh, ever-increasing EntryID,
     * but byte-identical stale content) - every reconnect piled one more
     * near-duplicate snapshot into the buffer, and replaying that pile
     * through the single-slot value-diff cache made long-settled values look
     * like they were changing again on every reconnect, well beyond what
     * actually changed during the outage. A buffered RCB's own EntryID
     * resume above already guarantees delivery of everything that happened
     * while disconnected - GI adds nothing there and is what was polluting
     * the backlog. GI is skipped in exactly that one case
     * (MmsReportClientUseCases_shouldRequestGiOnEnable) - buffered AND a
     * valid EntryID to resume from - and still requested unconditionally for
     * every unbuffered RCB (no buffer at all, so GI is the only way to catch
     * a change made while disconnected) and for a buffered RCB's very
     * first-ever enable / after an EntryID rejection resets the cache back
     * to NULL (nothing to resume from either way, same full-backlog safety
     * net as before this change). On the very first-ever connect, this
     * snapshot lands against the still-all-NULL cache and is silently
     * bootstrap-seeded (see shouldForwardAndUpdateCache's own doc comment).
     * `reason` is still never trusted for filtering (see
     * shouldForwardAndUpdateCache's own doc comment). TRG_OPS/BUF_TM/
     * INTG_PD/CONF_REV are still never touched - those stay exactly as the
     * IED's own SCL config already has them. */
    bool requestGi = MmsReportClientUseCases_shouldRequestGiOnEnable(target->buffered, hasResumableEntryId);
    if (requestGi) {
        ClientReportControlBlock_setGI(rcb, true);
        mask |= RCB_ELEMENT_GI;
    }

    IedConnection_setRCBValues(handle->connection, &err, rcb, mask, true);
    if (err != IED_ERROR_OK && (mask & RCB_ELEMENT_ENTRY_ID)) {
        /* The server may have rejected an EntryID it no longer recognizes -
         * e.g. its own buffer wrapped past it after a very long disconnect,
         * or the server itself restarted. IEC 61850 leaves the exact failure
         * mode here implementation-defined, so rather than guess at it, fall
         * back once to the pre-existing full-resume behavior instead of
         * leaving this RCB unreported. */
        fprintf(stderr, "[mms_report_client] setRCBValues with EntryID failed for '%s': error %d - "
                "retrying without EntryID (full resume, gi=true)\n", target->objectReference, err);
        mask &= ~(uint32_t) RCB_ELEMENT_ENTRY_ID;

        /* GI was skipped on the first attempt above precisely because we had
         * a resumable EntryID (requestGi was false) - now that the server
         * has rejected it and the cache is about to be cleared back to NULL,
         * this retry is a fresh full-resume enable with nothing to resume
         * from, so it needs the same GI safety net a genuine first-ever
         * enable gets. */
        if (!requestGi) {
            ClientReportControlBlock_setGI(rcb, true);
            mask |= RCB_ELEMENT_GI;
        }

        /* The server just told us the EntryID we cached is no longer valid
         * (buffer wrapped, or the server itself restarted with a fresh
         * EntryID counter) - clear it back to NULL rather than leaving it in
         * place. Required alongside MmsReportClientReportAdapter_onReport's
         * own EntryID-staleness guard (mms_report_client_report_adapter.c):
         * that guard drops any incoming report whose EntryID isn't strictly
         * greater than this cached value, and only ever advances it on a
         * report that survives the guard - so if a restarted server's own
         * counter legitimately restarts low, a stale cached value here would
         * make every one of its fresh reports look "stale" too, permanently
         * silencing this RCB until the whole daemon process restarts. This
         * reset is the one signal we actually have that the old baseline can
         * no longer be trusted, so it puts the guard back into its
         * fail-open, "nothing to compare against yet" bootstrap state. */
        if (target->buffered) {
            MmsReportClientMemberRefCacheEntry* cacheEntry =
                    lookupMemberRefCacheByRcb(handle, target->objectReference);
            if (cacheEntry) {
                Semaphore_wait(handle->memberRefCacheLock);
                if (cacheEntry->lastEntryId) {
                    MmsValue_delete(cacheEntry->lastEntryId);
                    cacheEntry->lastEntryId = NULL;
                }
                Semaphore_post(handle->memberRefCacheLock);
            }
        }

        IedConnection_setRCBValues(handle->connection, &err, rcb, mask, true);
    }
    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] setRCBValues failed for '%s': error %d\n",
                target->objectReference, err);
        IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false, err);
        }
        ClientReportControlBlock_destroy(rcb);
        return;
    }

    /* The one success-path log line in this function - every other fprintf
     * here fires only on failure, so today there is no way to see which RCBs
     * are actually reporting short of watching the IPC stream itself. */
    fprintf(stderr, "[mms_report_client] enabled reporting for '%s' (buffered=%d, dataset='%s', gi=%d)\n",
            target->objectReference, target->buffered,
            effectiveDatasetReference ? effectiveDatasetReference : "(none)", requestGi);

    if (handle->rcbStatusCallback) {
        handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, true, IED_ERROR_OK);
    }

    ClientReportControlBlock_destroy(rcb);
}

/*
 * Computes, once per connect cycle (called at the top of enableAllTargets),
 * a device-WIDE dataset plan covering every "Dyn" (SCL datSet="Dyn",
 * target->datasetReference == NULL) RCB slot on this IED - not just the ones
 * parented on an LN that itself has a Dyn RCB. A Dyn RCB's own parent LN does
 * NOT restrict what a dataset assigned to it can report on (dataset members
 * are independently addressed per-element, not tied to any one LN -
 * IedModel_getReportableAttributeReferencesForWholeDevice's own doc comment
 * has the full citation trail), so every Dyn RCB slot anywhere on the device
 * is treated as a fungible reporting channel that can be pointed at ANY part
 * of the device's reportable data - covering LDs/LNs that have zero RCBs of
 * their own (e.g. a real SIPROTEC device where ~28 of ~30 LDs have no RCB at
 * all, while one LD's LLN0 alone has dozens of otherwise-redundant spare RCB
 * instances that used to all just duplicate the same tiny LLN0-only dataset).
 *
 * Two clustering strategies, chosen by whether SCL declared a real
 * maxAttributes cap (IedModel_getDynDataSetMaxAttributes):
 *   - KNOWN (>0): the whole device's leaf list is packed via
 *     MmsReportClientUseCases_chunkReferencesAcrossWholeDevice - a single
 *     resulting cluster may legitimately span several different (small) LNs'
 *     worth of leaves when they fit together, maximizing how much of the
 *     device fits within a tight total dataset-count budget.
 *   - UNKNOWN (-1 or 0, e.g. no <Services> at all, or a dynamically-built
 *     online-discovered model): MmsReportClientUseCases_groupReferencesByLn
 *     packs one dataset per LN instead, unbounded size - the same per-LN
 *     granularity this feature always used before whole-device clustering
 *     existed, since combining multiple LNs into one dataset without a known
 *     size bound risks an oversized, doomed createDataSet call.
 *
 * Clusters are assigned to Dyn slots in simple model-declaration order
 * (handle->targets' own existing order - no attempt to prioritize which part
 * of the device matters more, there's no signal to rank by since this
 * feature never polls). Whichever list (clusters or slots) runs out first
 * determines the shortfall, logged plainly either way - never a silent drop:
 * more clusters than slots means part of the device goes unreported this
 * cycle; more slots than clusters means some RCB instances simply have
 * nothing left to assign (the device is already fully covered).
 *
 * Recomputed fresh every connect cycle rather than cached on the handle: a
 * pure function of already-static data (handle->iedModel, handle->targets,
 * both fixed for the client's whole lifetime), so recomputation is cheap
 * (string work over the model's own size, no wire calls) and idempotent -
 * not worth growing sMmsReportClientHandle for.
 */
static LinkedList
buildWholeDeviceClusterPlan(MmsReportClientHandle handle) {
    LinkedList plan = LinkedList_create();
    if (!handle->targets) return plan;

    /* Slot pool: every Dyn target on the WHOLE device, in handle->targets'
     * own existing order - a list of BORROWED ReportControlBlockTarget*
     * (owned by handle->targets itself), destroyed with
     * LinkedList_destroyStatic below, never Deep. Unlike the old per-LN
     * chunk plan, this is not filtered/grouped by LN at all - every Dyn
     * target anywhere on the device is one fungible slot in one combined
     * pool. */
    LinkedList slots = LinkedList_create();
    LinkedList slotScan = LinkedList_getNext(handle->targets);
    while (slotScan) {
        ReportControlBlockTarget* t = (ReportControlBlockTarget*) LinkedList_getData(slotScan);
        if (!t->datasetReference) LinkedList_add(slots, t);
        slotScan = LinkedList_getNext(slotScan);
    }
    if (LinkedList_size(slots) == 0) {
        LinkedList_destroyStatic(slots);
        return plan;
    }

    LinkedList wholeDeviceLeaves = IedModel_getReportableAttributeReferencesForWholeDevice(handle->iedModel);
    int leafCount = wholeDeviceLeaves ? LinkedList_size(wholeDeviceLeaves) : 0;
    if (leafCount == 0) {
        if (wholeDeviceLeaves) LinkedList_destroyDeep(wholeDeviceLeaves, free);
        LinkedList_destroyStatic(slots);
        return plan;
    }

    char** leafArray = calloc((size_t) leafCount, sizeof(char*));
    if (!leafArray) {
        LinkedList_destroyDeep(wholeDeviceLeaves, free);
        LinkedList_destroyStatic(slots);
        return plan;
    }
    int leafIdx = 0;
    for (LinkedList el = LinkedList_getNext(wholeDeviceLeaves); el; el = LinkedList_getNext(el)) {
        leafArray[leafIdx++] = (char*) LinkedList_getData(el);
    }

    int maxAttributes = IedModel_getDynDataSetMaxAttributes(handle->iedModel);
    LinkedList clusterLists = (maxAttributes > 0)
            ? MmsReportClientUseCases_chunkReferencesAcrossWholeDevice(
                    (const char* const*) leafArray, leafCount, maxAttributes)
            : MmsReportClientUseCases_groupReferencesByLn((const char* const*) leafArray, leafCount);
    free(leafArray);
    LinkedList_destroyDeep(wholeDeviceLeaves, free);

    int totalClusters = LinkedList_size(clusterLists);
    int slotCount = LinkedList_size(slots);
    int assignedClusters = 0;

    LinkedList clusterElement = LinkedList_getNext(clusterLists);
    LinkedList slotElement = LinkedList_getNext(slots);
    while (clusterElement) {
        if (!slotElement) {
            fprintf(stderr, "[mms_report_client] whole-device clustering produced %d dataset(s) but this "
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
                memberArray[m++] = MmsReportClientUtils_safeStringDup((char*) LinkedList_getData(mEl));
            }

            DynamicDatasetChunkAssignment* assignment = malloc(sizeof(DynamicDatasetChunkAssignment));
            if (assignment) {
                assignment->rcbReference = MmsReportClientUtils_safeStringDup(assignedTarget->objectReference);
                assignment->memberReferences = memberArray;
                assignment->memberCount = memberCount;
                LinkedList_add(plan, assignment);
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
        fprintf(stderr, "[mms_report_client] whole-device clustering produced %d dataset(s) for %d "
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

/* One bad RCB must not abort the rest - enableOneTarget catches its own
 * errors and enableAllTargets always visits every cached target, UNLESS a
 * concurrent stop request lands mid-loop (checked each iteration below).
 *
 * This matters on a device with many RCBs (confirmed against a real ~40-RCB
 * device): if the caller destroys this handle while this loop is still
 * mid-flight on a background supervisor thread (e.g. orchestration's
 * fail-hard rollback tearing down an already-started report client because a
 * later stage, GOOSE subscriber start, failed), MmsReportClientConnection_stop
 * sets stopRequested and closes the connection with no coordination against
 * this loop. Without this check, every remaining target after the one
 * in-flight at that moment fails immediately with IED_ERROR_CONNECTION_LOST -
 * a long, noisy, entirely wasted cascade. The one target already genuinely
 * in-flight when the close lands still fails (inherent, unavoidable without
 * deeper library-level synchronization) - this only stops the cascade past
 * that point. */
/* True if `datasetName` is the resolved dataset name of any entry already in
 * `cache` - used by cleanupOrphanedDatasets to recognize a discovered
 * existing dataset that a target already actively claimed/reused this
 * cycle (via tier 4's own create-or-reuse-on-exists path), as distinct from
 * one adoptUnclaimedDataset separately tracks in session->claimedDatasetNames. */
static bool
cacheContainsDatasetName(LinkedList cache, const char* datasetName) {
    if (!cache || !datasetName) return false;
    LinkedList element = LinkedList_getNext(cache);
    while (element) {
        DynamicDatasetCacheEntry* entry = (DynamicDatasetCacheEntry*) LinkedList_getData(element);
        if (entry->datasetName && strcmp(entry->datasetName, datasetName) == 0) return true;
        element = LinkedList_getNext(element);
    }
    return false;
}

/*
 * Proactive orphan cleanup, called at the end of every enableAllTargets:
 * reclaims budget from OUR OWN domain-scoped datasets sitting on the server
 * but not needed by any target this cycle - the ungraceful-restart gap
 * MmsReportClientConnection_stop's own cleanup-on-stop can't close on its
 * own (a killed/crashed daemon never reaches that code path at all - the
 * real-world scenario that motivated this whole discovery/reuse/cleanup
 * pass), now closed proactively on every successful connect instead of only
 * on a graceful one.
 *
 * STRICT, conservative safety bar: a discovered name is only ever deleted if
 * it exactly reconstructs via buildDynamicDatasetName(target->objectReference,
 * true) for some real buffered Dyn target in handle->targets right now -
 * never a name that merely looks like it might be ours, and never anything
 * that doesn't hit this exact bar. A foreign dataset, regardless of origin,
 * is never deleted here - only ever adopted (see adoptUnclaimedDataset's own
 * doc comment) - deletion is destructive and must stay limited to datasets
 * this client can prove it created itself.
 *
 * "Not needed this cycle" means the name never got claimed (adopted, or
 * found-unusable-and-skipped - either way tracked in
 * session->claimedDatasetNames) and never ended up in session->cache for its
 * own target's create-or-reuse resolution - i.e. genuinely idle leftover
 * capacity, not something actively serving a report right now. Best-effort:
 * a delete failure just leaves the dataset behind, logged, not fatal.
 */
static void
cleanupOrphanedDatasets(MmsReportClientHandle handle, DynamicDatasetSession* session) {
    if (!session->existingServerDatasets || !handle->targets) return;

    LinkedList element = LinkedList_getNext(session->existingServerDatasets);
    while (element) {
        char* candidate = (char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);

        if (stringListContains(session->claimedDatasetNames, candidate)) continue;
        if (cacheContainsDatasetName(session->cache, candidate)) continue;

        bool isOurs = false;
        LinkedList targetElement = LinkedList_getNext(handle->targets);
        while (targetElement) {
            ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(targetElement);
            targetElement = LinkedList_getNext(targetElement);
            if (!target->buffered || !target->objectReference) continue;

            char* expected = buildDynamicDatasetName(target->objectReference, true);
            bool match = expected && strcmp(expected, candidate) == 0;
            free(expected);
            if (match) {
                isOurs = true;
                break;
            }
        }
        if (!isOurs) continue;

        IedClientError deleteErr = IED_ERROR_OK;
        if (IedConnection_deleteDataSet(handle->connection, &deleteErr, candidate)) {
            fprintf(stderr, "[mms_report_client] cleaned up orphaned dataset '%s' - not needed by any "
                    "target this cycle, reclaiming budget\n", candidate);
        } else {
            fprintf(stderr, "[mms_report_client] could not clean up orphaned dataset '%s': error %d - "
                    "left behind\n", candidate, deleteErr);
        }
    }
}

static void
enableAllTargets(MmsReportClientHandle handle) {
    if (!handle->targets) return;

    /* Discovery happens before the session is even built, since its result
     * feeds the budget's own initial value - see discoverExistingServerDatasets'
     * and MmsReportClientUseCases_computeInitialDynamicDatasetBudget's own doc
     * comments. */
    LinkedList existingServerDatasets = discoverExistingServerDatasets(handle);

    /* Fresh per connect cycle, never carried across reconnects - see
     * DynamicDatasetSession's own doc comment. */
    DynamicDatasetSession session = {
        .cache = LinkedList_create(),
        .remainingBudget = MmsReportClientUseCases_computeInitialDynamicDatasetBudget(
                IedModel_getDynDataSetMax(handle->iedModel), LinkedList_size(existingServerDatasets)),
        .budgetExhaustedLogged = false,
        .chunkAssignments = buildWholeDeviceClusterPlan(handle),
        .existingServerDatasets = existingServerDatasets,
        .claimedDatasetNames = LinkedList_create(),
    };

    LinkedList element = LinkedList_getNext(handle->targets);
    while (element && !handle->stopRequested) {
        enableOneTarget(handle, (ReportControlBlockTarget*) LinkedList_getData(element), &session);
        element = LinkedList_getNext(element);
    }

    if (!handle->stopRequested) cleanupOrphanedDatasets(handle, &session);

    if (session.cache) LinkedList_destroyDeep(session.cache, destroyDynamicDatasetCacheEntry);
    if (session.chunkAssignments) LinkedList_destroyDeep(session.chunkAssignments, destroyDynamicDatasetChunkAssignment);
    if (session.existingServerDatasets) LinkedList_destroyDeep(session.existingServerDatasets, free);
    if (session.claimedDatasetNames) LinkedList_destroyDeep(session.claimedDatasetNames, free);
}

/* Sleeps in small chunks so MmsReportClientConnection_stop()'s bounded wait
 * for the supervisor to exit doesn't have to wait out a full backoff delay
 * (hal_thread.h has no interruptible sleep primitive). */
static void
interruptibleSleep(MmsReportClientHandle handle, uint32_t totalMs) {
    const uint32_t chunkMs = 100;
    uint32_t slept = 0;
    while (slept < totalMs && !handle->stopRequested) {
        uint32_t thisChunk = (totalMs - slept) < chunkMs ? (totalMs - slept) : chunkMs;
        Thread_sleep((int) thisChunk);
        slept += thisChunk;
    }
}

static void*
supervisorLoop(void* parameter) {
    MmsReportClientHandle handle = (MmsReportClientHandle) parameter;

    while (!handle->stopRequested) {
        IedClientError err = IED_ERROR_OK;
        IedConnection_connect(handle->connection, &err, handle->host, handle->port);

        if (err == IED_ERROR_OK) {
            handle->connectionRejectedSignaled = false;
            uint64_t connectedAtMs = Hal_getTimeInMs();
            enableAllTargets(handle);

            /* Stay in this connected phase, consuming every wake, until a
             * genuine connection-lost signal (or stop) arrives - do NOT loop
             * back to IedConnection_connect()/enableAllTargets() on every
             * wake. onStateChanged posts wakeSignal on EVERY state
             * transition, not just loss (see its own comment) -
             * IedConnection_connect() itself drives CONNECTING then
             * CONNECTED, each posting once, so by the time this thread first
             * reaches the wait below there are already pending posts left
             * over from the connect that just succeeded. Treating any single
             * wake as "go reconnect" (the old bare `continue` here) re-ran
             * enableAllTargets() - a second, entirely redundant RptEna
             * (enable) cycle per RCB - for one real connect, with no
             * connection having actually been lost. Confirmed as a
             * real-hardware root cause (see CLAUDE.md) of a duplicate-report
             * flood after reconnect. */
            for (;;) {
                Semaphore_wait(handle->wakeSignal);
                if (handle->stopRequested) break;
                if (handle->connectionLostSignal) {
                    handle->connectionLostSignal = false;
                    break;
                }
                /* spurious wake (a state-changed post not tied to loss, e.g.
                 * the connect sequence's own CONNECTING/CONNECTED posts) -
                 * keep waiting in this same connected phase. */
            }
            /* Reaching here past the inner loop means either stopRequested
             * (checked again below) or a genuine loss - a fresh association
             * means the server forgot our prior RptEna/report-handler
             * registration, so fall through to backoff + retry.
             *
             * Only reset the backoff to the initial tier if THIS connection
             * actually stayed up for a meaningful stretch
             * (MMS_REPORT_CLIENT_STABLE_CONNECTION_MS) - resetting
             * unconditionally on every momentary success (the old behavior)
             * meant a real, flaky link that connects then bounces right back
             * (unlike the clean loopback simulator, which never does this)
             * got stuck retrying at the initial ~1s tier forever instead of
             * escalating, letting enableAllTargets' full reset+GI cycle
             * (and everything it can spuriously forward - see
             * mms_report_client_usecases.c's valuesAreSemanticallyEqual doc
             * comment) fire repeatedly in a tight burst right after connect. */
            if (Hal_getTimeInMs() - connectedAtMs >= MMS_REPORT_CLIENT_STABLE_CONNECTION_MS) {
                handle->currentBackoffMs = 0;
            }
        } else if (err == IED_ERROR_CONNECTION_REJECTED && !handle->connectionRejectedSignaled
                && handle->connStateCallback) {
            /* Edge-triggered - see connectionRejectedSignaled's own doc comment
             * (mms_report_client_types.h) for why this only fires once per rejection
             * streak rather than every backoff cycle. Note onStateChanged (above)
             * already unconditionally fired MMS_REPORT_CLIENT_DISCONNECTED for this
             * same failed attempt via IED_STATE_CLOSED, synchronously during the
             * IedConnection_connect() call just above - this is an intentional,
             * harmless second callback invocation carrying more specific
             * information, not a duplicate-report bug. */
            handle->connectionRejectedSignaled = true;
            handle->connStateCallback(handle->connStateCallbackParam, MMS_REPORT_CLIENT_CONNECTION_REJECTED);
        }

        if (handle->stopRequested) break;

        uint32_t delay = MmsReportClientUseCases_computeNextBackoffDelay(
                handle->currentBackoffMs, handle->config.reconnectInitialDelayMs,
                handle->config.reconnectMaxDelayMs);
        handle->currentBackoffMs = delay;
        interruptibleSleep(handle, delay);
    }

    handle->supervisorExited = true;
    return NULL;
}

MmsReportClientError
MmsReportClientConnection_create(MmsReportClientHandle handle) {
    handle->connection = IedConnection_createEx(NULL, true);
    if (!handle->connection) return MMS_REPORT_CLIENT_ERR_OUT_OF_MEMORY;

    if (handle->config.connectTimeoutMs > 0) {
        IedConnection_setConnectTimeout(handle->connection, handle->config.connectTimeoutMs);
    }
    if (handle->config.requestTimeoutMs > 0) {
        IedConnection_setRequestTimeout(handle->connection, handle->config.requestTimeoutMs);
    }
    /* Applied once, here, before the connection is ever used - covers every
     * subsequent reconnect too, since supervisorLoop reuses this same
     * IedConnection object rather than recreating it per attempt (see
     * mms_report_client_auth.h). No-op if no password is configured. */
    MmsReportClientAuth_configurePasswordAuth(handle->connection, handle->config.acseAuthPassword);

    IedConnection_installStateChangedHandler(handle->connection, onStateChanged, handle);

    return MMS_REPORT_CLIENT_OK;
}

MmsReportClientError
MmsReportClientConnection_start(MmsReportClientHandle handle) {
    handle->wakeSignal = Semaphore_create(0);
    if (!handle->wakeSignal) return MMS_REPORT_CLIENT_ERR_OUT_OF_MEMORY;

    handle->memberRefCacheLock = Semaphore_create(1);
    if (!handle->memberRefCacheLock) {
        Semaphore_destroy(handle->wakeSignal);
        handle->wakeSignal = NULL;
        return MMS_REPORT_CLIENT_ERR_OUT_OF_MEMORY;
    }

    handle->stopRequested = false;
    handle->connectionLostSignal = false;
    handle->supervisorExited = false;
    handle->currentBackoffMs = 0;
    handle->connectionRejectedSignaled = false;

    handle->supervisorThread = Thread_create(supervisorLoop, handle, false);
    if (!handle->supervisorThread) {
        Semaphore_destroy(handle->wakeSignal);
        handle->wakeSignal = NULL;
        Semaphore_destroy(handle->memberRefCacheLock);
        handle->memberRefCacheLock = NULL;
        return MMS_REPORT_CLIENT_ERR_THREAD_CREATE_FAILED;
    }
    Thread_start(handle->supervisorThread);

    return MMS_REPORT_CLIENT_OK;
}

void
MmsReportClientConnection_stop(MmsReportClientHandle handle) {
    if (!handle || handle->stopRequested) return;

    handle->stopRequested = true;

    /* Buffered RCBs' self-created datasets are domain/VMD-scoped (see
     * getOrCreateDynamicDataset's own doc comment), so unlike the
     * association-scoped ones an unbuffered RCB gets, the server does NOT
     * clean these up automatically when this connection closes - this is the
     * only place that ever explicitly deletes them, and it must run BEFORE
     * IedConnection_close below (deleteDataSet needs a live association).
     * Without this, every start/stop cycle against the same device leaks one
     * more dataset into its own total dataset-count budget
     * (<Services><DynDataSet max="N"/>) until nothing is left for anything
     * else. Best-effort: a delete failure (already gone, device rejects
     * delete, connection already lost) just leaves a server-side dataset
     * behind - logged, not fatal to stopping. */
    if (handle->connection && handle->domainScopedDynamicDatasetNames) {
        LinkedList element = LinkedList_getNext(handle->domainScopedDynamicDatasetNames);
        while (element) {
            char* datasetName = (char*) LinkedList_getData(element);
            IedClientError deleteErr = IED_ERROR_OK;
            if (!IedConnection_deleteDataSet(handle->connection, &deleteErr, datasetName)) {
                fprintf(stderr, "[mms_report_client] could not delete dynamic dataset '%s' on stop: "
                        "error %d - left behind on the device\n", datasetName, deleteErr);
            }
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyDeep(handle->domainScopedDynamicDatasetNames, free);
        handle->domainScopedDynamicDatasetNames = NULL;
    }

    if (handle->connection) IedConnection_close(handle->connection);
    if (handle->wakeSignal) Semaphore_post(handle->wakeSignal);

    if (handle->supervisorThread) {
        while (!handle->supervisorExited) {
            Thread_sleep(20);
        }
    }

    if (handle->connection && handle->targets) {
        LinkedList element = LinkedList_getNext(handle->targets);
        while (element) {
            ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);
            IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
            element = LinkedList_getNext(element);
        }
    }
}

void
MmsReportClientConnection_destroy(MmsReportClientHandle handle) {
    if (!handle) return;

    /* Ordinarily already NULL - MmsReportClientConnection_stop (always
     * called first by MmsReportClient_destroy) already deleted every entry
     * server-side and freed this list. Defensive-only fallback for a
     * hypothetical future caller that destroys without stopping first: still
     * frees the local list (no local leak either way), just without the
     * server-side IedConnection_deleteDataSet calls - the connection may
     * already be gone by the time destroy runs. */
    if (handle->domainScopedDynamicDatasetNames) {
        LinkedList_destroyDeep(handle->domainScopedDynamicDatasetNames, free);
        handle->domainScopedDynamicDatasetNames = NULL;
    }

    if (handle->supervisorThread) {
        Thread_destroy(handle->supervisorThread);
        handle->supervisorThread = NULL;
    }
    /* IedConnection_destroy can synchronously re-fire onStateChanged (it
     * internally closes the connection again even if already closed by
     * MmsReportClientConnection_stop), which unconditionally posts
     * handle->wakeSignal - so the semaphore must still be alive when this
     * runs. Destroying it first (the previous order) left a freed semaphore
     * for that callback to post to, a use-after-free that crashed
     * intermittently whenever a caller destroyed an actively-connecting
     * client (e.g. orchestration rolling back an already-started
     * mms_report_client after a later stage fails). */
    if (handle->connection) {
        IedConnection_destroy(handle->connection);
        handle->connection = NULL;
    }
    if (handle->wakeSignal) {
        Semaphore_destroy(handle->wakeSignal);
        handle->wakeSignal = NULL;
    }
    if (handle->memberRefCacheLock) {
        Semaphore_destroy(handle->memberRefCacheLock);
        handle->memberRefCacheLock = NULL;
    }
}
