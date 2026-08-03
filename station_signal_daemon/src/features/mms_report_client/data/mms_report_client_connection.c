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

/* One entry per Logical Node whose RCB(s) needed a dynamically-created
 * dataset within the current connect cycle - see getOrCreateDynamicDataset's
 * own doc comment for why this de-dup exists. Built fresh in enableAllTargets
 * for every (re)connect and discarded at the end of that same call; never
 * carried across reconnects (the @-scoped datasets it names don't survive a
 * reconnect either - see MmsReportClientConnection_create's own comment on
 * the connection object's own reuse for the parallel reasoning). */
typedef struct {
    char* lnReference;  /* owned copy, matches ReportControlBlockTarget.lnReference */
    char* datasetName;  /* owned copy, e.g. "@dyn_E13_6MD_PTOC1" */
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
lookupDynamicDatasetName(LinkedList cache, const char* lnReference) {
    if (!cache || !lnReference) return NULL;

    LinkedList element = LinkedList_getNext(cache);
    while (element) {
        DynamicDatasetCacheEntry* entry = (DynamicDatasetCacheEntry*) LinkedList_getData(element);
        if (entry->lnReference && strcmp(entry->lnReference, lnReference) == 0) return entry->datasetName;
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

/* "@"-prefixed => association-scoped (destroyed automatically when this
 * connection closes - see IedConnection_createDataSet's own doc comment) -
 * no explicit delete needed, no risk of leaking the device's dataset budget
 * across reconnects/restarts. lnReference is sanitized ('/' -> '_') purely so
 * the generated name reads sensibly in logs; createDataSet doesn't require
 * any particular naming beyond the leading "@". */
static char*
buildDynamicDatasetName(const char* lnReference) {
    size_t len = strlen("@dyn_") + strlen(lnReference) + 1;
    char* name = malloc(len);
    if (!name) return NULL;
    snprintf(name, len, "@dyn_%s", lnReference);
    for (char* p = name; *p; p++) {
        if (*p == '/') *p = '_';
    }
    return name;
}

/*
 * For an RCB whose SCL declared no datSet (datasetReference == NULL,
 * datSet="Dyn" in SCL terms - see CLAUDE.md's own bullet on this): creates an
 * association-scoped dataset covering every FC=ST/MX leaf attribute under the
 * RCB's own LN (member list already resolved once, locally, into
 * handle->memberRefCache by buildMemberRefCache - see that function's own
 * comment on why the same list is reused here rather than re-walked).
 *
 * dynamicDatasetCache de-dupes by LN within one connect cycle: many real
 * IEDs expose several reserved RCB instances per LN (e.g. urcbA..urcbJ) that
 * would otherwise each trigger their own createDataSet call for what is
 * conceptually the same dataset - wasteful, and unnecessarily consumes the
 * device's (often small, e.g. 15) dataset-count budget.
 *
 * Returns the dataset name (borrowed - owned by dynamicDatasetCache, valid
 * for the rest of the current connect cycle) on success, or NULL if no
 * reportable attributes were found for this LN, or if dataset creation
 * itself failed (cap exceeded, maxAttributes exceeded, etc.) - either way,
 * the caller falls back to today's pre-existing behavior (skip DATSET, let
 * setRCBValues fail, log, move on).
 */
static const char*
getOrCreateDynamicDataset(MmsReportClientHandle handle, ReportControlBlockTarget* target,
        LinkedList dynamicDatasetCache) {
    if (!dynamicDatasetCache || !target->lnReference) return NULL;

    const char* existing = lookupDynamicDatasetName(dynamicDatasetCache, target->lnReference);
    if (existing) return existing;

    MmsReportClientMemberRefCacheEntry* cacheEntry = lookupMemberRefCacheByRcb(handle, target->objectReference);
    if (!cacheEntry || cacheEntry->memberCount <= 0) {
        fprintf(stderr, "[mms_report_client] no reportable (FC=ST/MX) attributes found for LN '%s' - "
                "'%s' will not get a dynamic dataset\n",
                target->lnReference, target->objectReference);
        return NULL;
    }

    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(
            (const char* const*) cacheEntry->memberReferences, cacheEntry->memberCount);
    if (!wireRefs || LinkedList_size(wireRefs) == 0) {
        fprintf(stderr, "[mms_report_client] no wire-convertible attribute references for LN '%s' - "
                "'%s' will not get a dynamic dataset\n",
                target->lnReference, target->objectReference);
        if (wireRefs) LinkedList_destroyDeep(wireRefs, free);
        return NULL;
    }

    char* datasetName = buildDynamicDatasetName(target->lnReference);
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
            datasetName, target->lnReference, cacheEntry->memberCount);

    IedClientError err = IED_ERROR_OK;
    IedConnection_createDataSet(handle->connection, &err, datasetName, wireRefs);
    LinkedList_destroyDeep(wireRefs, free);

    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] dynamic dataset creation failed for LN '%s': error %d - "
                "'%s' will not report\n", target->lnReference, err, target->objectReference);
        free(datasetName);
        return NULL;
    }

    fprintf(stderr, "[mms_report_client] created dynamic dataset '%s' for LN '%s'\n", datasetName,
            target->lnReference);

    DynamicDatasetCacheEntry* cacheNode = malloc(sizeof(DynamicDatasetCacheEntry));
    if (!cacheNode) {
        /* Dataset now exists on the server but there's no way to remember
         * the name for reuse this cycle - association-scoped, so it's
         * cleaned up automatically when this connection eventually closes;
         * just missing a reuse opportunity for the rest of this cycle, not a
         * leak. */
        free(datasetName);
        return NULL;
    }
    cacheNode->lnReference = MmsReportClientUtils_safeStringDup(target->lnReference);
    cacheNode->datasetName = datasetName;
    LinkedList_add(dynamicDatasetCache, cacheNode);

    return cacheNode->datasetName;
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
 * way (a dangling reference would fail to resolve there too). */
static bool
looksLikeOurOwnDynamicDatasetName(const char* datasetRef, const char* lnReference) {
    if (!datasetRef || !lnReference) return false;

    char* expected = buildDynamicDatasetName(lnReference);
    if (!expected) return false;

    bool match = (strcmp(datasetRef, expected) == 0);
    free(expected);
    return match;
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
    if (looksLikeOurOwnDynamicDatasetName(liveDataset, target->lnReference)) return false;

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

/*
 * Tier 3's counterpart of refreshPulledMemberRefCache, called once
 * effectiveDatasetReference is known to be getOrCreateDynamicDataset's own
 * deterministic "@dyn_<lnReference>" name (or NULL, if even self-creation
 * failed - a no-op, nothing to reconcile). Because that name/shape is 100%
 * deterministic per LN and was ALREADY built into this entry as the
 * LN-fallback PROVISIONAL shape at MmsReportClient_start time
 * (buildMemberRefCache, mms_report_client_api.c - tagged with a NULL
 * resolvedDatasetReference precisely so this first call always confirms it),
 * this is bookkeeping-only in the overwhelmingly common case: first-ever
 * successful tier-3 enable, or any reconnect after that (the name is
 * deterministic, so it matches every time) just stamps the fingerprint under
 * the lock - zero array rebuilding, zero extra calls beyond createDataSet
 * itself. The only case this actually rebuilds the shape is a transition
 * BACK from a previously-active tier-2 pulled dataset (resolvedDatasetReference
 * held some prior live dataset's name, now gone/unresolvable this cycle) - in
 * that one case, the LN-fallback shape is recomputed from scratch via
 * IedModel_getReportableAttributeReferencesForLogicalNode (purely local, no
 * wire call - the same offline call buildMemberRefCache itself makes) and
 * swapped in exactly like refreshPulledMemberRefCache's own shape-swap, with
 * the same value-diff-cache-reset consequence and the same rationale.
 */
static void
ensureLnFallbackMemberRefCache(MmsReportClientHandle handle, ReportControlBlockTarget* target,
        const char* effectiveDatasetReference) {
    if (!effectiveDatasetReference) return;

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

    LinkedList members = IedModel_getReportableAttributeReferencesForLogicalNode(handle->iedModel,
            target->lnReference);
    int count = members ? LinkedList_size(members) : 0;
    if (count <= 0) {
        if (members) LinkedList_destroyDeep(members, free);
        /* Nothing reportable for this LN (mirrors getOrCreateDynamicDataset's
         * own "no reportable attributes" log-and-skip posture) - still stamp
         * the fingerprint so this rebuild isn't retried on every single
         * enable. */
        Semaphore_wait(handle->memberRefCacheLock);
        free(entry->resolvedDatasetReference);
        entry->resolvedDatasetReference = MmsReportClientUtils_safeStringDup(effectiveDatasetReference);
        Semaphore_post(handle->memberRefCacheLock);
        return;
    }

    char** array = calloc((size_t) count, sizeof(char*));
    if (!array) {
        LinkedList_destroyDeep(members, free);
        return;
    }
    int i = 0;
    for (LinkedList el = LinkedList_getNext(members); el; el = LinkedList_getNext(el)) {
        array[i++] = MmsReportClientUtils_safeStringDup((char*) LinkedList_getData(el));
    }
    LinkedList_destroyDeep(members, free);

    MmsReportClientMemberRefCacheEntry* fresh = MmsReportClientUseCases_buildMemberRefCacheEntry(
            handle->iedModel, target->objectReference, array, count, effectiveDatasetReference);
    if (!fresh) return;

    Semaphore_wait(handle->memberRefCacheLock);
    MmsReportClientUseCases_swapMemberRefCacheEntryShape(entry, fresh);
    Semaphore_post(handle->memberRefCacheLock);
}

static void
enableOneTarget(MmsReportClientHandle handle, ReportControlBlockTarget* target, LinkedList dynamicDatasetCache) {
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
     * Three-tier resolution order for which dataset to assign:
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
     *   3. SELF-CREATE - only if both above are absent/unusable,
     *      getOrCreateDynamicDataset synthesizes one covering every
     *      reportable (FC=ST/MX) attribute of the RCB's own LN,
     *      association-scoped so it needs no explicit cleanup - UNCHANGED
     *      from this feature's original dynamic-dataset-creation behavior;
     *      this tier's own mechanism/lifetime is not touched by tiers 1-2
     *      existing above it, only the priority order is new.
     * If even tier 3 fails (no reportable attributes, or the device rejects
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
            effectiveDatasetReference = getOrCreateDynamicDataset(handle, target, dynamicDatasetCache);
            ensureLnFallbackMemberRefCache(handle, target, effectiveDatasetReference);
            datasetTier = "self-created";
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
static void
enableAllTargets(MmsReportClientHandle handle) {
    if (!handle->targets) return;

    /* Fresh per connect cycle, never carried across reconnects - see
     * DynamicDatasetCacheEntry's own doc comment. */
    LinkedList dynamicDatasetCache = LinkedList_create();

    LinkedList element = LinkedList_getNext(handle->targets);
    while (element && !handle->stopRequested) {
        enableOneTarget(handle, (ReportControlBlockTarget*) LinkedList_getData(element), dynamicDatasetCache);
        element = LinkedList_getNext(element);
    }

    if (dynamicDatasetCache) LinkedList_destroyDeep(dynamicDatasetCache, destroyDynamicDatasetCacheEntry);
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
