#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_report_client/data/mms_report_client_connection.h"
#include "features/mms_report_client/data/mms_report_client_report_adapter.h"
#include "features/mms_report_client/data/mms_report_client_auth.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"
#include "hal_time.h"
#include "mms_value.h"
#include "mms_client_connection.h"
#include "iec61850_common_internal.h"

/* A connection must stay up at least this long before a subsequent loss
 * resets the exponential backoff back to the initial tier - see
 * supervisorLoop's own comment on why. */
#define MMS_REPORT_CLIENT_STABLE_CONNECTION_MS 5000

/* IED_ERROR_TEMPORARILY_UNAVAILABLE on setRCBValues is a legitimate MMS
 * "try again shortly" signal, not a structural rejection - seen in practice
 * right after a real device's own reboot, while its MMS/report stack was
 * still finishing initialization. enableOneTarget gives it a few short,
 * bounded retries before falling through to the same terminal failure
 * handling as any other error (which would otherwise tear down a report
 * handler that, per real-hardware logs, can already be actively receiving
 * valid reports at that exact moment). */
#define MMS_REPORT_CLIENT_TEMP_UNAVAILABLE_MAX_RETRIES 3
#define MMS_REPORT_CLIENT_TEMP_UNAVAILABLE_RETRY_DELAY_MS 500

/* Defined further down in this file (used by supervisorLoop's own backoff
 * wait) - forward-declared here so enableOneTarget can reuse it for the
 * TEMPORARILY_UNAVAILABLE retry above without reordering the file. */
static void interruptibleSleep(MmsReportClientHandle handle, uint32_t totalMs);

/* Defined further down in this file (near cleanupOrphanedDatasets, its other
 * call site) - forward-declared here so createAndCacheDynamicDatasetAttempt's
 * stale-dataset-content fix can reuse it without reordering the file. See its
 * own doc comment further down for the full reasoning. */
static bool disableUnbindAndDeleteDataset(MmsReportClientHandle handle, const char* targetObjectReference,
        const char* datasetName);

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
 * connect cycle: the existing LN-keyed dedup cache, plus TWO budget counters
 * seeded from SCL's own <Services> declarations - one per dataset pool a
 * createDataSet attempt can actually draw from:
 *
 *   - remainingDynBudget: <DynDataSet max="N"/> (IedModel_getDynDataSetMax),
 *     UNCORRECTED - nothing this client can see ahead of time pre-exists in
 *     this pool (association-specific datasets live inside another
 *     connection's own private association state, invisible to
 *     discoverExistingServerDatasets' per-LD directory query - see that
 *     function's own doc comment). Decremented only on a genuinely new
 *     successful association-specific createDataSet.
 *   - remainingConfBudget: <ConfDataSet max="N"/> (IedModel_getConfDataSetMax),
 *     CORRECTED for datasets already discovered on the server
 *     (MmsReportClientUseCases_computeInitialDynamicDatasetBudget) at the top
 *     of enableAllTargets - not just a blind copy of the declared max, which
 *     has no awareness of what's already consuming the device's real budget
 *     (leftover domain-scoped datasets from an earlier ungracefully-terminated
 *     run, other clients'/tools' own datasets, etc. - a real device run showed
 *     exactly this silently exhausting the real budget while this client's own
 *     naive counter still believed most of it remained). This is the ONLY pool
 *     discovery can ever correct, since discovery can only ever see
 *     domain-scoped (Conf-class) datasets in the first place. Decremented on
 *     any genuinely new successful DOMAIN-SCOPED createDataSet - a buffered
 *     target's own primary attempt (always domain-scoped) and an unbuffered
 *     target's fallback attempt (createAndCacheDynamicDataset, once its real
 *     association-specific attempt is rejected) both draw from this same pool.
 *
 * Both model "how many MORE of our own new createDataSet attempts this
 * connect cycle may still make" in their own pool. -1 (SCL never declared a
 * cap) must never trigger either short-circuit - see
 * MmsReportClientUseCases_isDynamicDatasetBudgetExhausted. Neither is ever
 * decremented on a cache hit or an adopted existing dataset (both are free,
 * regardless of pool). dynBudgetExhaustedLogged/confBudgetExhaustedLogged
 * edge-trigger their own "stopping" log so each fires once per cycle per
 * pool, not once per remaining target - without this, a device with many
 * RCBs past a budget wall would print one near-identical line per remaining
 * target instead of one.
 *
 * Built fresh in enableAllTargets for every (re)connect, same lifetime as
 * DynamicDatasetCacheEntry's own cache - see that struct's doc comment for why
 * neither is carried across reconnects (existingServerDatasets/claimedDatasetNames
 * share the same reasoning: server-side state may have genuinely changed
 * since the last connect, so discovery re-runs fresh every time too).
 */
typedef struct {
    LinkedList cache;
    int remainingDynBudget;
    bool dynBudgetExhaustedLogged;
    int remainingConfBudget;
    bool confBudgetExhaustedLogged;
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
 * this path must never leave server-side state the rest of this client
 * doesn't know about, which is exactly the kind of leak the rest of this
 * feature works hard to avoid. */
static void
logGranularCreateDataSetError(MmsReportClientHandle handle, const char* datasetName,
        const char* const* memberReferences, int memberCount, bool buffered) {
    MmsConnection mmsConnection = IedConnection_getMmsConnection(handle->connection);
    if (!mmsConnection) return;

    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(memberReferences, memberCount);
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
        fprintf(stderr, "[mms_report_client] createDataSet diagnostic for '%s': raw MMS error = %s (%d)\n",
                datasetName, MmsError_toString(mmsError), (int) mmsError);

        if (mmsError == MMS_ERROR_NONE) {
            fprintf(stderr, "[mms_report_client] createDataSet diagnostic for '%s' UNEXPECTEDLY SUCCEEDED where "
                    "the real attempt just failed - deleting it now rather than leaving an untracked orphan on "
                    "the device\n", datasetName);
            IedClientError deleteErr = IED_ERROR_OK;
            if (!IedConnection_deleteDataSet(handle->connection, &deleteErr, datasetName)) {
                fprintf(stderr, "[mms_report_client] could not delete diagnostic dataset '%s': error %d - left "
                        "behind on the device\n", datasetName, deleteErr);
            }
        }
    }

    LinkedList_destroyDeep(specs, (LinkedListValueDeleteFunction) MmsVariableAccessSpecification_destroy);
}

/* Set-equality (order-insensitive) between a char** array (arrayCount
 * entries) and a LinkedList of char* - shared by refreshPulledMemberRefCache's
 * own content-staleness check (tiers 2/3) and verifyOrCorrectReusedDataset's
 * own OBJECT_EXISTS verification (tier 4) - both need "did the real member
 * set change under a name this client already has cached/is about to trust,"
 * just fed different inputs (a decode cache's own current shape vs. a
 * freshly-fetched dataset directory). Bounded by maxAttributes in practice
 * (realistically tens of members), so an O(n^2) scan is fine - no need for a
 * hash set here. */
static bool
memberReferenceSetsDiffer(const char* const* arrayRefs, int arrayCount, LinkedList listRefs) {
    if (arrayCount != (listRefs ? LinkedList_size(listRefs) : 0)) return true;
    for (int i = 0; i < arrayCount; i++) {
        bool found = false;
        LinkedList element = listRefs ? LinkedList_getNext(listRefs) : NULL;
        while (element) {
            if (strcmp((char*) LinkedList_getData(element), arrayRefs[i]) == 0) {
                found = true;
                break;
            }
            element = LinkedList_getNext(element);
        }
        if (!found) return true;
    }
    return false;
}

/* Forces ensureLnFallbackMemberRefCache's own needsRebuild check to fire on
 * its very next call for this RCB, regardless of whether the dataset NAME
 * changed - used after verifyOrCorrectReusedDataset below corrects a chunk's
 * member list in place: since the dataset name itself is unchanged (that's
 * the whole reason the corruption was invisible - see this feature's own
 * CHANGELOG entry on the stale-dataset-content bug), the plain name-compare
 * ensureLnFallbackMemberRefCache normally relies on would otherwise never
 * re-fire once already stamped from an earlier, wrong resolution. */
static void
invalidateMemberRefCacheFingerprint(MmsReportClientHandle handle, const char* rcbReference) {
    MmsReportClientMemberRefCacheEntry* entry = lookupMemberRefCacheByRcb(handle, rcbReference);
    if (!entry) return;
    Semaphore_wait(handle->memberRefCacheLock);
    free(entry->resolvedDatasetReference);
    entry->resolvedDatasetReference = NULL;
    Semaphore_post(handle->memberRefCacheLock);
}

typedef enum {
    STALE_DATASET_VERIFY_UNVERIFIABLE, /* couldn't fetch/convert the live directory - trust reuse unchanged,
                                           same fail-open posture as every other tier's own directory-fetch
                                           failure handling (pullLiveDataset/tryAdoptCandidate). */
    STALE_DATASET_VERIFY_OK,           /* content confirmed equal (or corrected in place to match reality) -
                                           caller proceeds exactly as today's plain reuse. */
    STALE_DATASET_VERIFY_DELETED,      /* content genuinely differed and the stale dataset was deleted -
                                           caller should retry createDataSet once, now genuinely fresh. */
} StaleDatasetVerifyResult;

/* The fix for this feature's one unverified reuse path: `datasetName` just
 * came back IED_ERROR_OBJECT_EXISTS from createDataSet, about to be trusted
 * outright on the (usually true, but NOT ALWAYS - see CHANGELOG.md)
 * assumption that a deterministic name implies deterministic content. This
 * fetches the dataset's REAL member list (IedConnection_getDataSetDirectory +
 * MmsReportClientUseCases_convertAcsiRefToMemberReference - identical
 * fetch+convert shape to tiers 2/3's own pullLiveDataset/tryAdoptCandidate)
 * and compares it, as a SET (order-insensitive - a benign reorder from an
 * older clustering algorithm run isn't dangerous once the cache is always
 * rebuilt from the fetched list below), against `requestedMemberReferences`
 * (this cycle's own locally-computed intent for this dataset).
 *
 * Equal: `chunkToCorrect` (may be NULL - defensive only, every real caller
 * has one) is overwritten in place with the FETCHED list, so the downstream
 * decode cache always matches real wire order regardless of whether it
 * already happened to - then the cache fingerprint is invalidated (see
 * invalidateMemberRefCacheFingerprint above) so that correction actually
 * gets picked up even on a connect cycle after the very first one. No delete
 * needed; caller treats this as a normal reuse.
 *
 * Different: this is the real bug case - some earlier connect cycle (an
 * older code version, or just a different run) created this exact
 * deterministic name with different content. Deletes it via
 * disableUnbindAndDeleteDataset (unbinding first is required - see that
 * function's own doc comment) so the caller can retry a genuinely fresh
 * create. If the delete itself fails (device refuses, e.g. still bound to
 * some OTHER RCB this client doesn't know about), falls back to reuse
 * anyway - `chunkToCorrect` is still overwritten with the fetched
 * (mismatched) list so the decode cache stays internally consistent (no
 * NULL-slot crash), but this RCB's real coverage now differs from what
 * clustering intended for it until a device-side reset, logged loudly.
 *
 * KNOWN E2E-COVERAGE GAP, documented rather than faked (same posture as
 * integration_tests/mms_report_client/e2e_test_mms_report_client.c's own
 * EntryID-staleness gap, see that file's top-of-file comment): reaching this
 * function at all requires tiers 2 (pullLiveDataset) and 3 (adoptUnclaimedDataset)
 * to BOTH miss a dataset that genuinely already exists on the server under
 * this exact name - against the vendored reference simulator, both tiers
 * reliably catch it first (tier 2 because RCB live state survives a
 * disconnect there; tier 3 because discoverExistingServerDatasets reliably
 * finds anything genuinely present), so this branch could not be forced
 * end-to-end without patching vendored third_party_src (a Hard Rule
 * violation). Verified instead by code review and by confirming the full
 * E2E suite still passes unchanged with this function in place. */
static StaleDatasetVerifyResult
verifyOrCorrectReusedDataset(MmsReportClientHandle handle, const char* cacheKey, const char* logRcbReference,
        const char* datasetName, const char* const* requestedMemberReferences, int requestedMemberCount,
        DynamicDatasetChunkAssignment* chunkToCorrect) {
    IedClientError err = IED_ERROR_OK;
    bool isDeletable = false;
    LinkedList acsiMembers = IedConnection_getDataSetDirectory(handle->connection, &err, datasetName, &isDeletable);
    if (!acsiMembers || LinkedList_size(acsiMembers) == 0) {
        fprintf(stderr, "[mms_report_client] could not verify existing dataset '%s' for '%s' (error %d) - "
                "reusing it unverified, same as before this check existed\n", datasetName, logRcbReference, err);
        if (acsiMembers) LinkedList_destroyDeep(acsiMembers, free);
        return STALE_DATASET_VERIFY_UNVERIFIABLE;
    }

    LinkedList fetchedMemberRefs = LinkedList_create();
    LinkedList acsiElement = LinkedList_getNext(acsiMembers);
    while (acsiElement) {
        char* acsiRef = (char*) LinkedList_getData(acsiElement);
        char* memberRef = MmsReportClientUseCases_convertAcsiRefToMemberReference(acsiRef);
        if (memberRef) LinkedList_add(fetchedMemberRefs, memberRef);
        acsiElement = LinkedList_getNext(acsiElement);
    }
    LinkedList_destroyDeep(acsiMembers, free);

    if (LinkedList_size(fetchedMemberRefs) == 0) {
        fprintf(stderr, "[mms_report_client] existing dataset '%s' for '%s' had no wire-convertible members "
                "on verification - reusing it unverified, same as before this check existed\n",
                datasetName, logRcbReference);
        LinkedList_destroyDeep(fetchedMemberRefs, free);
        return STALE_DATASET_VERIFY_UNVERIFIABLE;
    }

    bool setEqual = !memberReferenceSetsDiffer(requestedMemberReferences, requestedMemberCount, fetchedMemberRefs);

    if (!setEqual) {
        fprintf(stderr, "[mms_report_client] existing dataset '%s' for '%s' has %d member(s) on the server but "
                "this cycle's own computed content has %d - stale content from an earlier connect cycle, "
                "deleting and recreating\n", datasetName, logRcbReference, LinkedList_size(fetchedMemberRefs),
                requestedMemberCount);

        if (disableUnbindAndDeleteDataset(handle, cacheKey, datasetName)) {
            LinkedList_destroyDeep(fetchedMemberRefs, free);
            return STALE_DATASET_VERIFY_DELETED;
        }

        fprintf(stderr, "[mms_report_client] could not delete stale dataset '%s' for '%s' - reusing it anyway "
                "with corrected local decode shape; this RCB's real coverage differs from what was intended "
                "until a device-side reset\n", datasetName, logRcbReference);
    }

    /* Equal, or mismatched-but-undeletable: either way, make the local decode
     * shape match the FETCHED reality, not just re-stamp what was already
     * requested - covers a benign reorder too, not just the mismatch case. */
    if (chunkToCorrect) {
        for (int i = 0; i < chunkToCorrect->memberCount; i++) free(chunkToCorrect->memberReferences[i]);
        free(chunkToCorrect->memberReferences);

        int newCount = LinkedList_size(fetchedMemberRefs);
        char** newArray = newCount > 0 ? calloc((size_t) newCount, sizeof(char*)) : NULL;
        int ai = 0;
        LinkedList moveElement = LinkedList_getNext(fetchedMemberRefs);
        while (moveElement) {
            if (newArray) newArray[ai++] = (char*) LinkedList_getData(moveElement);
            else free(LinkedList_getData(moveElement));
            moveElement = LinkedList_getNext(moveElement);
        }
        chunkToCorrect->memberReferences = newArray;
        chunkToCorrect->memberCount = newArray ? newCount : 0;
        LinkedList_destroyStatic(fetchedMemberRefs); /* strings moved into newArray above, or freed inline */
    } else {
        LinkedList_destroyDeep(fetchedMemberRefs, free);
    }

    invalidateMemberRefCacheFingerprint(handle, cacheKey);
    return STALE_DATASET_VERIFY_OK;
}

static const char*
createAndCacheDynamicDatasetAttempt(MmsReportClientHandle handle, DynamicDatasetSession* session, const char* cacheKey,
        const char* logLnReference, const char* logRcbReference, const char* const* memberReferences,
        int memberCount, bool buffered, DynamicDatasetChunkAssignment* chunkToCorrectOnMismatch) {
    /* Which pool this attempt draws from is exactly what `buffered` already
     * means here - see DynamicDatasetSession's own doc comment. Checked
     * before any wire work so an already-exhausted pool costs nothing beyond
     * this one log line (edge-triggered, see that same doc comment). */
    int* budget = buffered ? &session->remainingConfBudget : &session->remainingDynBudget;
    bool* budgetExhaustedLogged = buffered ? &session->confBudgetExhaustedLogged : &session->dynBudgetExhaustedLogged;
    if (MmsReportClientUseCases_isDynamicDatasetBudgetExhausted(*budget)) {
        if (!*budgetExhaustedLogged) {
            int sclMax = buffered ? IedModel_getConfDataSetMax(handle->iedModel)
                                   : IedModel_getDynDataSetMax(handle->iedModel);
            fprintf(stderr, "[mms_report_client] %s budget (SCL %s max=%d) exhausted this connect cycle - no "
                    "further createDataSet attempts will be made; remaining RCB(s) needing a %s dataset will "
                    "not report\n", buffered ? "ConfDataSet" : "DynDataSet", buffered ? "ConfDataSet" : "DynDataSet",
                    sclMax, buffered ? "domain-scoped" : "association-specific");
            *budgetExhaustedLogged = true;
        }
        return NULL;
    }

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
     * already on the server. This USED TO be treated as a successful reuse
     * on the bare assumption that a deterministic name implies deterministic
     * content - proven false against a real device (see CHANGELOG.md's
     * stale-dataset-content entry: a prior connect cycle, under a different
     * clustering algorithm run, can leave this exact name behind with
     * DIFFERENT content) - so this now actually verifies before trusting it,
     * the same way tiers 2/3 already verify a pulled/adopted dataset's real
     * content instead of just assuming it. */
    bool reusedExisting = false;
    if (err == IED_ERROR_OBJECT_EXISTS && buffered) {
        StaleDatasetVerifyResult verifyResult = verifyOrCorrectReusedDataset(handle, cacheKey, logRcbReference,
                datasetName, memberReferences, memberCount, chunkToCorrectOnMismatch);

        if (verifyResult == STALE_DATASET_VERIFY_DELETED) {
            /* Stale predecessor deleted - retry the create once, now
             * genuinely fresh (wireRefs above was already destroyed, so
             * rebuilt fresh here; cheap and only ever reached on this rare,
             * already-logged-loudly path). */
            LinkedList retryWireRefs = MmsReportClientUseCases_buildWireMemberReferences(memberReferences, memberCount);
            if (retryWireRefs && LinkedList_size(retryWireRefs) > 0) {
                IedConnection_createDataSet(handle->connection, &err, datasetName, retryWireRefs);
            } else {
                err = IED_ERROR_OBJECT_VALUE_INVALID; /* couldn't even rebuild the wire form - terminal below */
            }
            if (retryWireRefs) LinkedList_destroyDeep(retryWireRefs, free);

            if (err == IED_ERROR_OK) {
                fprintf(stderr, "[mms_report_client] recreated dynamic dataset '%s' for LN '%s' after deleting "
                        "its stale, mismatched-content predecessor\n", datasetName, logLnReference);
                /* reusedExisting stays false - this is a genuinely new create, falls through to the normal
                 * "created" bookkeeping (budget decrement, cache insert) below exactly like any other. */
            } else {
                fprintf(stderr, "[mms_report_client] recreate after stale-dataset delete failed for '%s': "
                        "error %d - '%s' will not report\n", datasetName, err, logRcbReference);
                free(datasetName);
                return NULL;
            }
        } else {
            fprintf(stderr, "[mms_report_client] dynamic dataset '%s' for LN '%s' already exists on the server - "
                    "reusing it\n", datasetName, logLnReference);
            reusedExisting = true;
        }
    } else if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] dynamic dataset creation failed for LN '%s': error %d - "
                "'%s' will not report\n", logLnReference, err, logRcbReference);
        logGranularCreateDataSetError(handle, datasetName, memberReferences, memberCount, buffered);
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
        if (*budget > 0) (*budget)--;
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

/* Some real devices (confirmed against a real SIPROTEC 6MD: every
 * association-specific create rejected with MMS reject-other, not a
 * resource/quota-class ServiceError, independent of remaining budget - see
 * CHANGELOG.md) reject association-specific ("@"-prefixed) dataset creation
 * outright, structurally, regardless of capacity. A domain-scoped dataset
 * assigned to an unbuffered RCB is otherwise perfectly valid IEC 61850 (this
 * client already does exactly that via the ADOPT tier whenever an existing
 * one is found) - only the CREATION form was ever the problem for such a
 * device, not the assignment. So an unbuffered target whose real
 * association-specific attempt fails gets one fallback attempt as a
 * domain-scoped dataset instead, accepting the same lifecycle tradeoff a
 * buffered target's dataset already has: it persists past this connection
 * and won't be auto-cleaned by the server, so it's tracked via
 * rememberDomainScopedDatasetName exactly like a buffered target's own
 * (createAndCacheDynamicDatasetAttempt keys every naming/reuse/cleanup
 * decision off its own `buffered` parameter, never off the target's real
 * buffered-ness, so calling it a second time with buffered=true here "just
 * works," including OBJECT_EXISTS-as-reuse on a later reconnect once this
 * fallback dataset already exists on the server).
 *
 * A buffered target already always uses the domain-scoped path on its one
 * and only attempt (see the four-tier resolution order's own comment in
 * enableOneTarget) - `buffered` short-circuits below because there is no
 * "more scoped" fallback left to try. */
static const char*
createAndCacheDynamicDataset(MmsReportClientHandle handle, DynamicDatasetSession* session, const char* cacheKey,
        const char* logLnReference, const char* logRcbReference, const char* const* memberReferences,
        int memberCount, bool buffered, DynamicDatasetChunkAssignment* chunkToCorrectOnMismatch) {
    const char* result = createAndCacheDynamicDatasetAttempt(handle, session, cacheKey, logLnReference,
            logRcbReference, memberReferences, memberCount, buffered, chunkToCorrectOnMismatch);
    if (result || buffered) return result;

    fprintf(stderr, "[mms_report_client] association-specific dataset creation failed for LN '%s' - falling "
            "back to a domain-scoped dataset instead (some real devices reject association-specific creation "
            "outright, independent of quota - see CHANGELOG.md) - this dataset will NOT be cleaned up "
            "automatically and permanently consumes device quota until a device-side reset\n", logLnReference);
    return createAndCacheDynamicDatasetAttempt(handle, session, cacheKey, logLnReference, logRcbReference,
            memberReferences, memberCount, true, chunkToCorrectOnMismatch);
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

    /* Cache hits above stay free (zero wire cost, always allowed) - the
     * budget check itself now lives in createAndCacheDynamicDatasetAttempt,
     * keyed per-attempt on which pool that attempt actually draws from (see
     * DynamicDatasetSession's own doc comment) - a single top-level check
     * here couldn't distinguish an unbuffered target's Dyn-budget-gated real
     * attempt from its own Conf-budget-gated fallback. */
    return createAndCacheDynamicDataset(handle, session, target->objectReference, target->lnReference,
            target->objectReference, (const char* const*) chunk->memberReferences, chunk->memberCount,
            target->buffered, chunk);
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
 * ONLY MEANINGFUL FOR UNBUFFERED TARGETS: this whole "dangling" rationale
 * rests on the association-scoped ("@"-prefixed) name being destroyed the
 * instant the prior connection closed. A buffered target's own dataset is
 * domain/VMD-scoped instead and genuinely PERSISTS past a connection close -
 * that persistence is the entire reason buildDynamicDatasetName gives
 * buffered targets a different naming scheme in the first place (see
 * enableOneTarget's tier-4 doc bullet). So a buffered target's own name
 * showing up as its RCB's live DatSet on reconnect is not dangling at all -
 * it's the expected, persistent, reusable case, and rejecting it here was a
 * real bug: it forced every buffered Dyn RCB's reconnect through tier 3
 * (adoptUnclaimedDataset) instead of the cheap, correct tier-2 reuse path,
 * and (compounded by adoptUnclaimedDataset having no preference for a
 * target's own name - see that function's own doc comment) could cause
 * sibling RCBs under the same LD to cross-adopt each other's datasets on
 * every reconnect, spuriously resetting their value-diff caches back to
 * bootstrap and permanently suppressing real reports.
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
    if (target->buffered) return false;

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
 * the live RCB value is the only signal consulted. This is also the common
 * reconnect-time path for a BUFFERED target reusing its own domain-scoped
 * dataset from a prior connect cycle, now that looksLikeOurOwnDynamicDatasetName
 * no longer rejects it (see that function's own doc comment) - the cheap,
 * correct "nothing to do" case this tier exists to serve.
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
 * On success, registers `liveDataset` into session->claimedDatasetNames -
 * the same claim-tracking tier 3/4 already self-register into before
 * returning. Without this, cleanupOrphanedDatasets (end of this same
 * enableAllTargets cycle) would see this name as unclaimed and delete it out
 * from under the RCB that is, at that very moment, actively reporting
 * through it - a real bug this closes: a reused live dataset is just as
 * "needed this cycle" as an adopted or self-created one, it was simply
 * invisible to that bookkeeping before.
 *
 * Returns true and fills *outMemberRefs (LinkedList of owned "$"-joined,
 * LD-prefixed member-reference char*, this feature's own memberReferences[]
 * convention) on success. Returns false (*outMemberRefs left untouched) if no
 * DatSet is currently assigned, it looks like our own dangling name (see
 * looksLikeOurOwnDynamicDatasetName - unbuffered targets only), or the live
 * fetch itself fails or yields zero wire-convertible members - the caller
 * falls through to getOrCreateDynamicDataset unchanged in every one of these
 * cases, exactly as if this tier didn't exist.
 */
static bool
pullLiveDataset(MmsReportClientHandle handle, ReportControlBlockTarget* target, ClientReportControlBlock rcb,
        DynamicDatasetSession* session, LinkedList* outMemberRefs) {
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

    if (session) {
        LinkedList_add(session->claimedDatasetNames, MmsReportClientUtils_safeStringDup(liveDataset));
    }

    *outMemberRefs = wireRefs;
    return true;
}

/*
 * Rebuilds (or confirms up to date) this RCB's memberRefCache entry to match
 * `liveDataset`'s real shape - called from enableOneTarget on every
 * (re)connect where pullLiveDataset (tier 2) or adoptUnclaimedDataset (tier 3)
 * succeeded - both feed this same function their own freshly-fetched real
 * member list (memberRefs), never trusted blindly.
 *
 * Rebuild decision has TWO parts, not just one: (1) compares liveDataset
 * against the cache entry's own resolvedDatasetReference fingerprint
 * (MmsReportClientMemberRefCacheEntry's own doc comment) - a NAME change
 * always forces a rebuild outright; (2) even when the name is UNCHANGED,
 * also compares `memberRefs` (this call's freshly-fetched real content)
 * against the cache entry's own currently-cached member list
 * (memberReferenceSetsDiffer) - a same-name dataset whose real content
 * silently changed since it was last cached (e.g. deleted and recreated
 * differently by an earlier connect cycle, another tool, or a device-side
 * reset, while this client's own in-memory cache never got the memo) is
 * just as stale as a name change, and is exactly the mechanism documented in
 * CHANGELOG.md's stale-dataset-content entry - a name-only check alone
 * cannot see it. Only the common case - name unchanged AND content
 * unchanged - is a true no-op, the expected steady state on every reconnect
 * once a stable dataset has been pulled once.
 *
 * Either check tripping means the previously-cached shape can no longer be
 * trusted: rebuilds a fresh
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
    bool nameChanged = !entry->resolvedDatasetReference || strcmp(entry->resolvedDatasetReference, liveDataset) != 0;
    /* Same-name reuse is NOT automatically safe - see this function's own
     * doc comment on the stale-dataset-content fix. Only checked when the
     * name matches (a name CHANGE already forces a rebuild on its own,
     * regardless of content) - cheap, since it's the common no-op case that
     * needs to stay fast on every reconnect. */
    bool contentChanged = !nameChanged
            && memberReferenceSetsDiffer((const char* const*) entry->memberReferences, entry->memberCount, memberRefs);
    bool needsRebuild = nameChanged || contentChanged;
    char* previousDatasetReference = (needsRebuild && entry->resolvedDatasetReference)
            ? MmsReportClientUtils_safeStringDup(entry->resolvedDatasetReference) : NULL;
    Semaphore_post(handle->memberRefCacheLock);
    if (!needsRebuild) return;

    if (contentChanged) {
        fprintf(stderr, "[mms_report_client] '%s' dataset '%s' content changed since it was last cached "
                "(same name, different real member list) - rebuilding decode shape\n",
                target->objectReference, liveDataset);
    }

    int count = memberRefs ? LinkedList_size(memberRefs) : 0;
    if (count <= 0) {
        free(previousDatasetReference);
        return;
    }

    char** array = calloc((size_t) count, sizeof(char*));
    if (!array) {
        free(previousDatasetReference);
        return;
    }
    int i = 0;
    for (LinkedList el = LinkedList_getNext(memberRefs); el; el = LinkedList_getNext(el)) {
        array[i++] = MmsReportClientUtils_safeStringDup((char*) LinkedList_getData(el));
    }

    MmsReportClientMemberRefCacheEntry* fresh = MmsReportClientUseCases_buildMemberRefCacheEntry(
            handle->iedModel, target->objectReference, array, count, liveDataset);
    if (!fresh) {
        free(previousDatasetReference);
        return;
    }

    Semaphore_wait(handle->memberRefCacheLock);
    MmsReportClientUseCases_swapMemberRefCacheEntryShape(entry, fresh);
    Semaphore_post(handle->memberRefCacheLock);

    /* Deliberately logged - see this function's own doc comment on why a
     * shape rebuild resets the value-diff cache back to bootstrap. Without
     * this line, that reset was completely silent, making it indistinguishable
     * in a log capture from every report simply always being "unchanged." */
    fprintf(stderr, "[mms_report_client] '%s' dataset identity changed (was '%s', now '%s') - "
            "value-diff cache reset to bootstrap\n", target->objectReference,
            previousDatasetReference ? previousDatasetReference : "(none)", liveDataset);
    free(previousDatasetReference);
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
 * Resolves one candidate dataset name for adoption: fetches its real member
 * list (IedConnection_getDataSetDirectory) and converts it to this feature's
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
tryAdoptCandidate(MmsReportClientHandle handle, ReportControlBlockTarget* target, DynamicDatasetSession* session,
        const char* candidate, LinkedList* outMemberRefs) {
    IedClientError err = IED_ERROR_OK;
    bool isDeletable = false;
    LinkedList acsiMembers = IedConnection_getDataSetDirectory(handle->connection, &err, candidate, &isDeletable);
    if (!acsiMembers || LinkedList_size(acsiMembers) == 0) {
        fprintf(stderr, "[mms_report_client] discovered dataset '%s' could not be resolved (error %d) - "
                "skipping as an adoption candidate\n", candidate, err);
        if (acsiMembers) LinkedList_destroyDeep(acsiMembers, free);
        LinkedList_add(session->claimedDatasetNames, MmsReportClientUtils_safeStringDup(candidate));
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
        return false;
    }

    fprintf(stderr, "[mms_report_client] adopting existing dataset '%s' for '%s' (%d member(s)) - "
            "reused instead of self-creating\n", candidate, target->objectReference, LinkedList_size(wireRefs));

    LinkedList_add(session->claimedDatasetNames, MmsReportClientUtils_safeStringDup(candidate));
    *outMemberRefs = wireRefs;
    return true;
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
 * PREFERS THIS EXACT TARGET'S OWN PREVIOUSLY-ASSIGNED NAME FIRST (buffered
 * targets only - buildDynamicDatasetName's domain-scoped naming is the only
 * scheme that can persist/be rediscovered across cycles at all): if
 * buildDynamicDatasetName(target->objectReference, true) is among this
 * cycle's discovered candidates and still unclaimed, it's tried before the
 * general LD-wide scan below. This is secondary hardening, independent of
 * pullLiveDataset (tier 2) already being the primary path for a buffered
 * target reclaiming its own dataset - without it, this target's own name
 * could otherwise be claimed by a DIFFERENT sibling RCB under the same LD
 * during the general scan below (which has no such preference), forcing
 * THIS target to self-create instead of reusing a dataset it already owns,
 * and - the real risk - mismatching the SIBLING's own resolvedDatasetReference
 * fingerprint, spuriously resetting the sibling's value-diff cache back to
 * bootstrap.
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

    if (target->buffered && target->objectReference) {
        char* ownName = buildDynamicDatasetName(target->objectReference, true);
        if (ownName) {
            LinkedList element = LinkedList_getNext(session->existingServerDatasets);
            while (element) {
                char* candidate = (char*) LinkedList_getData(element);
                element = LinkedList_getNext(element);
                if (strcmp(candidate, ownName) != 0) continue;
                if (!stringListContains(session->claimedDatasetNames, candidate)
                        && tryAdoptCandidate(handle, target, session, candidate, outMemberRefs)) {
                    free(ownName);
                    return candidate;
                }
                break;
            }
            free(ownName);
        }
    }

    LinkedList element = LinkedList_getNext(session->existingServerDatasets);
    while (element) {
        char* candidate = (char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);

        if (strncmp(candidate, target->lnReference, ldLen) != 0 || candidate[ldLen] != '/') continue;
        if (stringListContains(session->claimedDatasetNames, candidate)) continue;

        if (tryAdoptCandidate(handle, target, session, candidate, outMemberRefs)) return candidate;
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
    char* previousDatasetReference = (needsRebuild && entry->resolvedDatasetReference)
            ? MmsReportClientUtils_safeStringDup(entry->resolvedDatasetReference) : NULL;
    if (!needsRebuild) {
        Semaphore_post(handle->memberRefCacheLock);
        return;
    }
    Semaphore_post(handle->memberRefCacheLock);

    DynamicDatasetChunkAssignment* chunk = lookupChunkAssignment(session->chunkAssignments, target->objectReference);
    if (!chunk) {
        free(previousDatasetReference);
        return;
    }

    int count = chunk->memberCount;
    char** array = NULL;
    if (count > 0) {
        array = calloc((size_t) count, sizeof(char*));
        if (!array) {
            free(previousDatasetReference);
            return;
        }
        for (int i = 0; i < count; i++) {
            array[i] = MmsReportClientUtils_safeStringDup(chunk->memberReferences[i]);
        }
    }

    if (count <= 0) {
        /* Nothing reportable (mirrors getOrCreateDynamicDataset's own
         * "no reportable attributes" log-and-skip posture) - still stamp the
         * fingerprint so this rebuild isn't retried on every single enable.
         * No actual cache reset happens here (no fresh entry is built/swapped
         * below), so no reset log line - see the real swap path below. */
        Semaphore_wait(handle->memberRefCacheLock);
        free(entry->resolvedDatasetReference);
        entry->resolvedDatasetReference = MmsReportClientUtils_safeStringDup(effectiveDatasetReference);
        Semaphore_post(handle->memberRefCacheLock);
        free(previousDatasetReference);
        return;
    }

    MmsReportClientMemberRefCacheEntry* fresh = MmsReportClientUseCases_buildMemberRefCacheEntry(
            handle->iedModel, target->objectReference, array, count, effectiveDatasetReference);
    if (!fresh) {
        free(previousDatasetReference);
        return;
    }

    Semaphore_wait(handle->memberRefCacheLock);
    MmsReportClientUseCases_swapMemberRefCacheEntryShape(entry, fresh);
    Semaphore_post(handle->memberRefCacheLock);

    /* Deliberately logged - see refreshPulledMemberRefCache's own doc comment
     * on why a shape rebuild resets the value-diff cache back to bootstrap.
     * Without this line, that reset was completely silent, making it
     * indistinguishable in a log capture from every report simply always
     * being "unchanged." */
    fprintf(stderr, "[mms_report_client] '%s' dataset identity changed (was '%s', now '%s') - "
            "value-diff cache reset to bootstrap\n", target->objectReference,
            previousDatasetReference ? previousDatasetReference : "(none)", effectiveDatasetReference);
    free(previousDatasetReference);
}

/* Writes one step of an RCB's ordered enable sequence (see enableOneTarget's
 * own doc comment on why DatSet/RptEna/TrgOps are now three separate steps,
 * not one combined write) and unconditionally logs the outcome - added
 * specifically so a real device's per-step behavior is diagnosable from a
 * log capture instead of only ever seeing one final combined result.
 * Retries exactly like the old single-write path used to: bundled first
 * (singleRequest=true), then, if that failed, as separate sequential
 * per-element writes (singleRequest=false - some real devices reject a
 * bundle outright regardless of the values, see CHANGELOG.md), then a few
 * short retries if the device reports IED_ERROR_TEMPORARILY_UNAVAILABLE
 * (still coming up after a restart). `stepName` is purely a log label. */
static IedClientError
writeRcbStep(MmsReportClientHandle handle, ClientReportControlBlock rcb, const char* objectReference,
        uint32_t mask, const char* stepName) {
    IedClientError err = IED_ERROR_OK;
    IedConnection_setRCBValues(handle->connection, &err, rcb, mask, true);

    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] setRCBValues step '%s' (bundled) failed for '%s': error %d - "
                "retrying as sequential per-element writes\n", stepName, objectReference, err);
        IedConnection_setRCBValues(handle->connection, &err, rcb, mask, false);
    }

    int tempUnavailableRetries = 0;
    while (err == IED_ERROR_TEMPORARILY_UNAVAILABLE
            && tempUnavailableRetries < MMS_REPORT_CLIENT_TEMP_UNAVAILABLE_MAX_RETRIES
            && !handle->stopRequested) {
        tempUnavailableRetries++;
        fprintf(stderr, "[mms_report_client] setRCBValues step '%s' temporarily unavailable for '%s' (device "
                "likely still initializing after a restart) - retry %d/%d\n", stepName, objectReference,
                tempUnavailableRetries, MMS_REPORT_CLIENT_TEMP_UNAVAILABLE_MAX_RETRIES);
        interruptibleSleep(handle, MMS_REPORT_CLIENT_TEMP_UNAVAILABLE_RETRY_DELAY_MS);
        IedConnection_setRCBValues(handle->connection, &err, rcb, mask, true);
    }

    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] setRCBValues step '%s' failed for '%s': error %d\n",
                stepName, objectReference, err);
    } else {
        fprintf(stderr, "[mms_report_client] setRCBValues step '%s' succeeded for '%s'\n", stepName, objectReference);
    }

    return err;
}

/* Reads the RCB straight back from the device and logs exactly what it
 * reports for the three attributes this feature's enable sequence cares
 * about - RptEna/DatSet/TrgOps - as its own, separate MMS read, independent
 * of whatever this client just tried to write. Exists purely as a diagnostic:
 * a setRCBValues call can return IED_ERROR_OK while the device still didn't
 * actually apply what was asked (confirmed against a real device that
 * accepted a write with success but left an unbuffered RCB not actually
 * active) - without a real read-back there is no way to tell "we wrote it
 * and it stuck" from "we wrote it and the device silently ignored it" from
 * a log capture alone. Best-effort only: a failed read-back here is logged
 * and otherwise ignored, never treated as the enable itself failing. */
static void
logRcbLiveState(MmsReportClientHandle handle, const char* objectReference, const char* when) {
    IedClientError err = IED_ERROR_OK;
    ClientReportControlBlock live = IedConnection_getRCBValues(handle->connection, &err, objectReference, NULL);
    if (!live) {
        fprintf(stderr, "[mms_report_client] '%s' live read-back (%s) failed: error %d\n",
                objectReference, when, err);
        return;
    }
    const char* liveDataset = ClientReportControlBlock_getDataSetReference(live);
    fprintf(stderr, "[mms_report_client] '%s' device-reported live state (%s): RptEna=%d DatSet='%s' TrgOps=0x%x\n",
            objectReference, when, ClientReportControlBlock_getRptEna(live),
            liveDataset ? liveDataset : "(empty)", ClientReportControlBlock_getTrgOps(live));
    ClientReportControlBlock_destroy(live);
}

/* Reads the RCB's live TrgOps straight back from the device, for comparison
 * against what this client intended to set - see enableOneTarget's own
 * step-3 doc comment for why this check exists (a real device was found
 * reporting IED_ERROR_OK for a TrgOps write it then silently never applied).
 * Returns -1 (never a real TrgOps bitmask - always non-negative wire values)
 * on a failed read, so the caller's bit-mask comparison conservatively
 * treats an unreadable device as "didn't stick" rather than assuming success. */
static int
readLiveTrgOps(MmsReportClientHandle handle, const char* objectReference) {
    IedClientError err = IED_ERROR_OK;
    ClientReportControlBlock live = IedConnection_getRCBValues(handle->connection, &err, objectReference, NULL);
    if (!live) return -1;
    int trgOps = ClientReportControlBlock_getTrgOps(live);
    ClientReportControlBlock_destroy(live);
    return trgOps;
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
     * detected and logged as a bug. `mask` here accumulates only the
     * "config" elements written in step 1 (OptFlds/DatSet/EntryID) - RptEna
     * and TrgOps get their own masks for their own later steps, see this
     * function's own doc comment below on why the enable sequence is now
     * three separate ordered writes instead of one combined one. */
    uint32_t mask = 0;

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

    /* Proactively OR in TrgOps.dchg/qchg/gi for every RCB, rather than
     * relying on however the device happens to already be configured - a
     * real device (via OMICRON IED Scout's "Simulate IED" feature, standing
     * in for this device's real engineering export) was found with TrgOps
     * carrying ONLY General Interrogation (dchg/qchg/dupd/integrity all
     * false), meaning it would NEVER generate a report on an actual data or
     * quality change - only the one-time GI snapshot on enable, which this
     * feature's own bootstrap-suppression correctly never forwards anyway.
     * Every subsequent value change on such an RCB was therefore silently
     * invisible, with nothing to log on either side - the server genuinely
     * never sends anything, so there is nothing for this feature's own
     * reporting/filtering logic to even see, let alone drop.
     *
     * ORs TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI into
     * whatever TrgOps bits the device already has configured (read via
     * ClientReportControlBlock_getTrgOps, which reflects the device's own
     * current config since `rcb` was just populated from a real
     * IedConnection_getRCBValues call above) - never clobbers the rest, same
     * minimal-footprint posture as OptFlds.EntryID above. GI is included
     * here too since this feature's own GI request (RCB_ELEMENT_GI below)
     * depends on TrgOps.gi being enabled server-side to be honored at all,
     * per IEC 61850 - without it, even the bootstrap snapshot this feature
     * already relies on could be silently ignored by a spec-compliant
     * server. Deliberately does NOT touch TRG_OPT_DATA_UPDATE or
     * TRG_OPT_INTEGRITY: integrity is a periodic/timer-based trigger, and
     * this feature is deliberately, strictly event-driven (see
     * CHANGELOG.md) - enabling it would reintroduce exactly the kind of
     * "periodic traffic that looks like an event" problem the value-diff
     * cache exists to filter out, not something worth manufacturing on
     * purpose. Only writes it back (and only then adds RCB_ELEMENT_TRG_OPS
     * to the mask) if at least one of these bits isn't already set, to
     * avoid touching this attribute on every single reconnect once the
     * device has accepted it once - same reasoning as OptFlds.EntryID.
     * Bundled into step 1 (before enable) since most servers - including
     * this codebase's own `ied_simulator` - only accept a TrgOps change
     * while RptEna is false, and reject it with IED_ERROR_TEMPORARILY_UNAVAILABLE
     * once enabled (confirmed directly: `integration_tests/mms_report_client`'s
     * `test_dynamicDataset_giOnlyRcb_reportsRealChangeAfterTrgOpsFix` fails
     * with exactly that error if this write is deferred to after RptEna).
     * trgOpsNeedsUpdate is kept for a second purpose below - see this
     * function's own doc comment on the post-enable read-back/corrective
     * retry, for the opposite real-device failure mode (write reports
     * success here but silently never applies). */
    int currentTrgOps = ClientReportControlBlock_getTrgOps(rcb);
    int neededTrgOps = TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI;
    bool trgOpsNeedsUpdate = (currentTrgOps & neededTrgOps) != neededTrgOps;
    /* Logged unconditionally, not just on failure - a real device was found
     * with TrgOps reading back 0x0 at the very end of enable for some RCBs
     * but not others, reproducibly across separate connect cycles, and
     * nothing in this function previously logged the INITIAL value read here
     * (before any write) or distinguished "never needed a write" from "wrote
     * it, step 3's check passed, but it still ended up 0x0 anyway" - both
     * looked identical from the logs alone. This line plus step 3's own
     * now-unconditional logging (below) closes that gap. */
    fprintf(stderr, "[mms_report_client] '%s' initial live TrgOps=0x%x, needed=0x%x, update needed=%d\n",
            target->objectReference, currentTrgOps, neededTrgOps, trgOpsNeedsUpdate);
    if (trgOpsNeedsUpdate) {
        ClientReportControlBlock_setTrgOps(rcb, currentTrgOps | neededTrgOps);
        mask |= RCB_ELEMENT_TRG_OPS;
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
     *      start time - refreshPulledMemberRefCache reconciles that. Also the
     *      genuine reconnect-time path for a BUFFERED target's own
     *      domain-scoped dataset, which persists past a connection close and
     *      is therefore correctly recognized here (not rejected as dangling -
     *      see looksLikeOurOwnDynamicDatasetName's own doc comment) - a
     *      buffered target's reconnect now reuses its persisted dataset via
     *      this cheap tier (getDataSetDirectory only, no createDataSet call)
     *      instead of always falling through to tier 3/4.
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
        if (pullLiveDataset(handle, target, rcb, session, &pulledMemberRefs)) {
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
    if (!effectiveDatasetReference) {
        /* No tier resolved a dataset - setRCBValues below is guaranteed to
         * fail (a real device returns IED_ERROR_OBJECT_ATTRIBUTE_INCONSISTENT/31
         * for RptEna requested with no DatSet bound, confirmed against a real
         * SIPROTEC 6MD device), so skip the doomed write rather than making
         * it: against an already-struggling real device, one real MMS write
         * per unresolved RCB (potentially 100+ per connect cycle) is pure
         * noise indistinguishable from hammering it, for a result already
         * known here. rcbStatusCallback still fires with the same error code
         * a real attempt would have returned, so callers can't tell the
         * difference. */
        fprintf(stderr, "[mms_report_client] '%s' skipped - no dataset available, not attempting setRCBValues\n",
                target->objectReference);
        IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false,
                    IED_ERROR_OBJECT_ATTRIBUTE_INCONSISTENT);
        }
        ClientReportControlBlock_destroy(rcb);
        return;
    }
    ClientReportControlBlock_setDataSetReference(rcb, effectiveDatasetReference);
    mask |= RCB_ELEMENT_DATSET;

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
     * shouldForwardAndUpdateCache's own doc comment). TrgOps.dchg/qchg/gi are
     * proactively OR'd in via trgOpsNeedsUpdate above (see that block's own
     * doc comment) - BUF_TM/INTG_PD/CONF_REV, and TrgOps.dupd/integrity
     * specifically, are still never touched, staying exactly as the IED's
     * own config has them. requestGi is mutable (not const) - the EntryID
     * rejection retry below can still force it true after this point, same
     * as before this function's three-step restructuring. */
    bool requestGi = MmsReportClientUseCases_shouldRequestGiOnEnable(target->buffered, hasResumableEntryId);

    /* Real-device finding: a device can report IED_ERROR_OK for a TrgOps
     * write bundled alongside DatSet (step 1, below - still the primary,
     * pre-enable path, since most servers - including this codebase's own
     * `ied_simulator` - only accept TrgOps while RptEna is false, see that
     * step's own doc comment above) while silently never actually applying
     * it - a real device's unbuffered RCBs were found accepting that exact
     * write yet never actually going active. Rather than pick one fixed
     * order and risk being wrong for whichever kind of device is on the
     * other end, DatSet/RptEna/TrgOps are now three separate, independently
     * logged writes (step 1, step 2, and a conditional step 3 below), and
     * step 3 doesn't trust step 1's own claimed success for TrgOps - it
     * reads the RCB straight back off the device and only retries TrgOps
     * (now that RptEna is confirmed true, which some devices apparently
     * require) if the live value proves step 1 didn't actually stick. This
     * self-corrects for either failure mode observed in practice - a device
     * that only accepts TrgOps before enable (the common case, step 1 alone
     * suffices) or one that silently drops a pre-enable TrgOps write (step 3
     * catches and corrects it) - without assuming either behavior up front.
     * logRcbLiveState calls throughout are purely diagnostic, added
     * specifically so a failure like this one shows the device's own actual
     * reported state instead of only this client's guess at what it asked
     * for. */

    /* Step 1: dataset binding (DatSet, + TrgOps/EntryID/OptFlds if
     * applicable, all accumulated into `mask` above) - config sent before
     * the RCB is enabled, since most servers require that order. */
    err = writeRcbStep(handle, rcb, target->objectReference, mask, "DatSet");
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
         * step 2 below needs the same GI safety net a genuine first-ever
         * enable gets. */
        requestGi = true;

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
        /* This same rejection is also the definitive signal the device's
         * report state was reset (most commonly a real device reboot) - a
         * subsequently recreated dataset holds genuinely fresh values even
         * when it has the exact same deterministic name as before (the
         * everyday reconnect case ensureLnFallbackMemberRefCache's own
         * name-based check is built for), so the value-diff cache must be
         * reset to bootstrap here too, not just lastEntryId - otherwise the
         * next report diffs fresh post-reset values against stale
         * pre-reset ones and nearly everything reads as "changed."
         * MmsReportClientUseCases_resetValueDiffCacheToBootstrap's own doc
         * comment has the full reasoning. */
        if (target->buffered) {
            MmsReportClientMemberRefCacheEntry* cacheEntry =
                    lookupMemberRefCacheByRcb(handle, target->objectReference);
            if (cacheEntry) {
                Semaphore_wait(handle->memberRefCacheLock);
                if (cacheEntry->lastEntryId) {
                    MmsValue_delete(cacheEntry->lastEntryId);
                    cacheEntry->lastEntryId = NULL;
                }
                MmsReportClientUseCases_resetValueDiffCacheToBootstrap(cacheEntry);
                Semaphore_post(handle->memberRefCacheLock);
                fprintf(stderr, "[mms_report_client] '%s' EntryID rejected as no-longer-existing - value-diff "
                        "cache reset to bootstrap (device report state was reset)\n", target->objectReference);
            }
        }

        err = writeRcbStep(handle, rcb, target->objectReference, mask, "DatSet (EntryID retry)");
    }

    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] setRCBValues failed for '%s': error %d\n",
                target->objectReference, err);
        logRcbLiveState(handle, target->objectReference, "after failed DatSet step");
        IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false, err);
        }
        ClientReportControlBlock_destroy(rcb);
        return;
    }

    /* Step 2: enable - RptEna (+ GI, to force a deterministic snapshot per
     * requestGi's own doc comment above). This is the step that actually
     * makes the RCB active; step 3 below verifies step 1's TrgOps actually
     * stuck now that this has run. */
    ClientReportControlBlock_setRptEna(rcb, true);
    uint32_t enableMask = RCB_ELEMENT_RPT_ENA;
    if (requestGi) {
        ClientReportControlBlock_setGI(rcb, true);
        enableMask |= RCB_ELEMENT_GI;
    }

    err = writeRcbStep(handle, rcb, target->objectReference, enableMask, "RptEna");
    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] setRCBValues failed for '%s': error %d\n",
                target->objectReference, err);
        logRcbLiveState(handle, target->objectReference, "after failed RptEna step");
        IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false, err);
        }
        ClientReportControlBlock_destroy(rcb);
        return;
    }

    /* Step 3 (corrective, conditional): verify TrgOps actually stuck, and
     * retry it now that RptEna is confirmed true if it didn't. Step 1's
     * TrgOps write (above, before enable) is what most devices need - it's
     * what this codebase's own `ied_simulator` requires, and what a
     * combined/library-ordered write already did before this function's
     * three-step restructuring. But a real device was found reporting
     * IED_ERROR_OK for that exact pre-enable write while silently never
     * applying it - TrgOps only actually took effect once written again
     * AFTER RptEna was already true. A device's own claimed success is
     * therefore not trusted here: read the RCB straight back and check
     * whether its live TrgOps genuinely contains the bits this feature
     * needs before concluding step 1 actually worked. Only ever runs at all
     * if trgOpsNeedsUpdate was true to begin with (step 1 had something to
     * write) - most devices never reach this check. A failure here is
     * logged loudly but deliberately non-fatal to the overall enable: the
     * RCB is already active (RptEna=true, dataset bound) by this point, and
     * failing to also get TrgOps updated only means this specific device
     * may not generate change-triggered reports on this RCB, not that
     * reporting is broken outright. */
    if (trgOpsNeedsUpdate) {
        int liveTrgOps = readLiveTrgOps(handle, target->objectReference);
        /* Logged unconditionally (both branches), not just on mismatch - see
         * the initial-read log line above for why: without logging the
         * MATCH case too, "step 1 write never needed a retry" and "we simply
         * never checked because trgOpsNeedsUpdate was false" were
         * indistinguishable from a log capture alone. */
        fprintf(stderr, "[mms_report_client] '%s' step 3 check: live TrgOps=0x%x right after RptEna enable "
                "(needed=0x%x)\n", target->objectReference, liveTrgOps, neededTrgOps);
        if ((liveTrgOps & neededTrgOps) != neededTrgOps) {
            fprintf(stderr, "[mms_report_client] '%s' step 1's TrgOps write reported success but the device's "
                    "live TrgOps (0x%x) doesn't reflect the requested bits (0x%x) - retrying TrgOps now that "
                    "RptEna is enabled\n", target->objectReference, liveTrgOps, neededTrgOps);
            ClientReportControlBlock_setTrgOps(rcb, currentTrgOps | neededTrgOps);
            IedClientError trgOpsErr = writeRcbStep(handle, rcb, target->objectReference, RCB_ELEMENT_TRG_OPS,
                    "TrgOps (post-enable corrective retry)");
            if (trgOpsErr != IED_ERROR_OK) {
                fprintf(stderr, "[mms_report_client] '%s' post-enable TrgOps corrective retry also failed: "
                        "error %d - RCB stays enabled with whatever TrgOps the device already has; this device "
                        "may never generate change-triggered reports on this RCB\n", target->objectReference,
                        trgOpsErr);
            }
        }
    }

    logRcbLiveState(handle, target->objectReference, "after enable sequence");

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
 * maxAttributes cap - preferring ConfDataSet's own declared cap
 * (IedModel_getConfDataSetMaxAttributes) over DynDataSet's
 * (IedModel_getDynDataSetMaxAttributes), falling back to Dyn's if Conf isn't
 * declared: every buffered target's self-created dataset is always
 * domain-scoped (Conf-class) regardless, and on a device that structurally
 * rejects association-specific creation (confirmed against a real SIPROTEC
 * 6MD - see CHANGELOG.md), so does every unbuffered target's dataset once it
 * falls back (createAndCacheDynamicDataset). Sizing the ONE shared plan
 * against the larger, more-often-relevant cap directly shrinks the total
 * number of clusters needed for full-device coverage. The one accepted
 * tradeoff: a target whose real association-specific attempt would have
 * succeeded at Dyn's smaller cap, but is now sized above it, spends one
 * extra doomed attempt before correctly falling back to Conf - not a
 * correctness bug (the fallback already handles arbitrary Dyn failures),
 * just a narrow case traded for fewer datasets overall on devices where Dyn
 * creation never succeeds anyway.
 *   - KNOWN (>0): the whole device's leaf list is packed via
 *     MmsReportClientUseCases_buildBreadthFirstPerLnClusters, bounded both by
 *     maxAttributes (never spans two LNs in one dataset - see that
 *     function's own doc comment for why this changed from the old
 *     cross-LN-merging packer) AND by `explicitClusterBudget` (this cycle's
 *     real remaining Dyn+Conf dataset-count budget, computed by the caller
 *     BEFORE this function runs - see enableAllTargets - so clustering
 *     spends that real budget maximizing DISTINCT LN coverage instead of
 *     minimizing total dataset count the way the old cross-LN packer did.
 *     Found against a real device where the old minimal-count strategy only
 *     ever enabled ~14 of 178 real RCBs - the device's own leaf data fit in
 *     14 datasets, but the device's REAL dataset-count budget (SCL DynDataSet
 *     max + ConfDataSet max) was ~45, headroom the old strategy never spent.
 *   - UNKNOWN (-1 or 0 on both caps, e.g. no <Services> at all, or a
 *     dynamically-built online-discovered model): MmsReportClientUseCases_groupReferencesByLn
 *     packs one dataset per LN instead, unbounded size, uncapped by
 *     explicitClusterBudget too (no known size bound to safely sub-chunk an
 *     oversized LN against, so there is nothing this cap could safely trim
 *     without risking splitting one LN's own atomic DO group) - the same
 *     per-LN granularity this feature always used before whole-device
 *     clustering existed.
 *
 * NOTE: explicitClusterBudget is a planning-time upper bound only, meant to
 * stop wildly overplanning (e.g. building hundreds of never-fundable chunk
 * structs) - it is NOT the authoritative budget gate. That remains
 * createAndCacheDynamicDatasetAttempt's own per-attempt, per-pool check
 * (buffered draws from Conf, unbuffered from Dyn falling back to Conf), which
 * this cap can only ever overestimate (dynBudget+confBudget can't predict a
 * specific cluster's real pool split ahead of assignment) - never read this
 * cap as a promise that every planned cluster will actually get created.
 *
 * Clusters are assigned to Dyn slots LN-KEYED, not blind positional pairing:
 * each cluster (LN-homogeneous by construction in the KNOWN branch, always
 * was in the UNKNOWN branch) first tries to claim a still-unused slot whose
 * OWN target->lnReference matches the cluster's LN, falling back to any
 * remaining unused slot (any LN, handle->targets' own order) only once that
 * LN's own slots are exhausted - so "an LN's own RCB instance reports that
 * LN's own data" whenever that LN has a spare slot, rather than depending on
 * two independently-ordered lists (handle->targets' declaration order vs.
 * the leaf walk's own order) happening to line up by coincidence. Whichever
 * list (clusters or slots) runs out first determines the shortfall, logged
 * plainly either way - never a silent drop: more clusters than slots means
 * part of the device goes unreported this cycle; more slots than clusters
 * means some RCB instances simply have nothing left to assign (the device is
 * already fully covered, or explicitClusterBudget capped how much of it
 * could be planned this cycle).
 *
 * Recomputed fresh every connect cycle rather than cached on the handle: a
 * pure function of already-static data (handle->iedModel, handle->targets,
 * both fixed for the client's whole lifetime) plus this cycle's own budget
 * snapshot, so recomputation is cheap (string work over the model's own
 * size, no wire calls) and idempotent - not worth growing
 * sMmsReportClientHandle for.
 */
/* "LD/LN" prefix only (everything before the first '$') from one "$"-joined
 * member reference - mirrors mms_report_client_usecases.c's own file-local
 * extractLnKey (not exported; this feature's own established convention of
 * small, data-layer-local duplicates over cross-file sharing - see e.g.
 * lookupMemberRefCacheByRcb's own doc comment). Used to find which LN a
 * breadth-first cluster belongs to, for LN-keyed slot assignment below.
 * Caller owns the returned string (free); NULL input returns NULL. */
static char*
extractLnKeyFromMemberReference(const char* memberReference) {
    if (!memberReference) return NULL;
    const char* fcStart = strchr(memberReference, '$');
    size_t len = fcStart ? (size_t) (fcStart - memberReference) : strlen(memberReference);
    char* key = malloc(len + 1);
    if (!key) return NULL;
    memcpy(key, memberReference, len);
    key[len] = '\0';
    return key;
}

static LinkedList
buildWholeDeviceClusterPlan(MmsReportClientHandle handle, int explicitClusterBudget) {
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

    /* Prefer ConfDataSet's own declared maxAttributes over DynDataSet's - see
     * this function's own doc comment for why. Falls back to DynDataSet's
     * (today's behavior) if Conf isn't declared, then to per-LN grouping
     * (below) if neither is - same UNKNOWN posture as before this change.
     * KNOWN case now spends explicitClusterBudget on breadth (distinct-LN
     * coverage) rather than minimizing total dataset count - see this
     * function's own doc comment. UNKNOWN case is uncapped, unchanged. */
    int confMaxAttributes = IedModel_getConfDataSetMaxAttributes(handle->iedModel);
    int dynMaxAttributes = IedModel_getDynDataSetMaxAttributes(handle->iedModel);
    int maxAttributes = confMaxAttributes > 0 ? confMaxAttributes : dynMaxAttributes;
    LinkedList clusterLists = (maxAttributes > 0)
            ? MmsReportClientUseCases_buildBreadthFirstPerLnClusters(
                    (const char* const*) leafArray, leafCount, maxAttributes, explicitClusterBudget)
            : MmsReportClientUseCases_groupReferencesByLn((const char* const*) leafArray, leafCount);
    free(leafArray);
    LinkedList_destroyDeep(wholeDeviceLeaves, free);

    int totalClusters = LinkedList_size(clusterLists);
    int slotCount = LinkedList_size(slots);
    int assignedClusters = 0;

    /* LN-keyed slot assignment (see this function's own doc comment) - plain
     * arrays for easy "first still-unused slot matching this LN" scanning;
     * slotCount is realistically hundreds at most, so an O(slotCount) scan
     * per cluster is cheap (not worth a hash map here). */
    ReportControlBlockTarget** slotArray = calloc((size_t) slotCount, sizeof(ReportControlBlockTarget*));
    bool* slotUsed = calloc((size_t) slotCount, sizeof(bool));
    if (slotArray && slotUsed) {
        int si = 0;
        for (LinkedList el = LinkedList_getNext(slots); el; el = LinkedList_getNext(el)) {
            slotArray[si++] = (ReportControlBlockTarget*) LinkedList_getData(el);
        }
    }

    LinkedList clusterElement = LinkedList_getNext(clusterLists);
    while (clusterElement) {
        LinkedList clusterMembers = (LinkedList) LinkedList_getData(clusterElement);

        char* clusterLnKey = NULL;
        LinkedList firstMemberElement = clusterMembers ? LinkedList_getNext(clusterMembers) : NULL;
        if (firstMemberElement) {
            clusterLnKey = extractLnKeyFromMemberReference((char*) LinkedList_getData(firstMemberElement));
        }

        ReportControlBlockTarget* assignedTarget = NULL;
        if (slotArray && slotUsed) {
            if (clusterLnKey) {
                for (int i = 0; i < slotCount; i++) {
                    if (!slotUsed[i] && slotArray[i]->lnReference
                            && strcmp(slotArray[i]->lnReference, clusterLnKey) == 0) {
                        assignedTarget = slotArray[i];
                        slotUsed[i] = true;
                        break;
                    }
                }
            }
            if (!assignedTarget) {
                for (int i = 0; i < slotCount; i++) {
                    if (!slotUsed[i]) {
                        assignedTarget = slotArray[i];
                        slotUsed[i] = true;
                        break;
                    }
                }
            }
        }
        free(clusterLnKey);

        if (!assignedTarget) {
            fprintf(stderr, "[mms_report_client] whole-device clustering produced %d dataset(s) but this "
                    "device only has %d RCB instance(s) with no SCL-assigned dataset - %d cluster(s) "
                    "(part of the device's data) will not be reported this cycle\n",
                    totalClusters, slotCount, totalClusters - assignedClusters);
            break;
        }

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
    }

    if (!clusterElement && assignedClusters < slotCount) {
        fprintf(stderr, "[mms_report_client] whole-device clustering produced %d dataset(s) for %d "
                "available RCB instance(s) - %d instance(s) have nothing left to report (the device's "
                "own reportable data is already fully covered)\n",
                totalClusters, slotCount, slotCount - assignedClusters);
    }

    free(slotArray);
    free(slotUsed);

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
 * Shared single-pair disable-unbind-delete primitive: if `target`'s CURRENT
 * live DatSet is `datasetName`, disables RptEna and clears DatSet on it first
 * (both required before deletion - disabling RptEna alone is not enough,
 * confirmed against the reference server, IED_ERROR_OBJECT_CONSTRAINT_CONFLICT/35,
 * see test_deleteDataSet_refusedWhileRcbEnabled_succeedsAfterDisable), then
 * attempts IedConnection_deleteDataSet. Best-effort throughout: any failure
 * just leaves the dataset behind, logged via the returned false, never fatal
 * to the caller.
 *
 * Extracted from cleanupOrphanedDatasets' own per-candidate body (still its
 * only other reasoning source below) - MmsReportClientConnection_stop's own
 * bulk two-pass version (disable+unbind EVERY target against EVERY tracked
 * name in one pass, then delete every tracked name in a second) is a
 * different enough shape (bulk vs. single pair) that folding it into this
 * same single-pair primitive isn't attempted here; this repo's own
 * convention already tolerates this class of small duplication (see e.g.
 * lookupMemberRefCacheByRcb's own doc comment on the ACSE-auth-setup
 * duplication precedent).
 *
 * Takes the target's own objectReference directly (not a whole
 * ReportControlBlockTarget*) - the only field either caller actually needs,
 * and the stale-dataset-content fix in createAndCacheDynamicDatasetAttempt
 * only has that string on hand, not a target pointer.
 */
static bool
disableUnbindAndDeleteDataset(MmsReportClientHandle handle, const char* targetObjectReference,
        const char* datasetName) {
    IedClientError disableErr = IED_ERROR_OK;
    ClientReportControlBlock rcb = IedConnection_getRCBValues(handle->connection, &disableErr,
            targetObjectReference, NULL);
    if (rcb) {
        const char* liveDataSet = ClientReportControlBlock_getDataSetReference(rcb);
        if (liveDataSet && strcmp(liveDataSet, datasetName) == 0) {
            ClientReportControlBlock_setRptEna(rcb, false);
            ClientReportControlBlock_setDataSetReference(rcb, "");
            IedConnection_setRCBValues(handle->connection, &disableErr, rcb,
                    RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_DATSET, true);
            if (disableErr != IED_ERROR_OK) {
                fprintf(stderr, "[mms_report_client] could not disable/unbind '%s' (dataset '%s') "
                        "before delete: error %d\n", targetObjectReference, datasetName, disableErr);
            } else {
                fprintf(stderr, "[mms_report_client] disabled/unbound '%s' from dataset '%s' before delete\n",
                        targetObjectReference, datasetName);
            }
        }
        ClientReportControlBlock_destroy(rcb);
    }

    IedClientError deleteErr = IED_ERROR_OK;
    if (!IedConnection_deleteDataSet(handle->connection, &deleteErr, datasetName)) {
        fprintf(stderr, "[mms_report_client] could not delete dataset '%s': error %d - left behind\n",
                datasetName, deleteErr);
        return false;
    }
    return true;
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
 * capacity, not something actively serving a report right now. Before
 * deleting, disables RptEna and clears DatSet on the matched target's RCB if
 * (and only if) its live DatSet still is this candidate - same fix as
 * MmsReportClientConnection_stop's own graceful cleanup, needed here too
 * since a crashed daemon never reached that code path to do it itself, and
 * a still-live RptEna/DatSet blocks deletion regardless of which code path
 * eventually attempts it. Best-effort throughout: any failure just leaves
 * the dataset behind, logged, not fatal.
 */
static void
cleanupOrphanedDatasets(MmsReportClientHandle handle, DynamicDatasetSession* session, int* outLeakedCount) {
    if (!session->existingServerDatasets || !handle->targets) return;

    fprintf(stderr, "[mms_report_client] orphan cleanup: evaluating %d dataset(s) discovered on the server "
            "this cycle\n", LinkedList_size(session->existingServerDatasets));

    LinkedList element = LinkedList_getNext(session->existingServerDatasets);
    while (element) {
        char* candidate = (char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);

        if (stringListContains(session->claimedDatasetNames, candidate)) continue;
        if (cacheContainsDatasetName(session->cache, candidate)) continue;

        bool isOurs = false;
        ReportControlBlockTarget* matchedTarget = NULL;
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
                matchedTarget = target;
                break;
            }
        }
        if (!isOurs) continue;

        /* Same fix as MmsReportClientConnection_stop's own graceful cleanup:
         * a crashed daemon never reached that code path at all, so
         * matchedTarget's RptEna/DatSet may genuinely still be live
         * server-side here exactly as if this were the graceful path - this
         * proactive pass needs the identical disable+unbind before its own
         * delete attempt, now shared via disableUnbindAndDeleteDataset (see
         * that function's own doc comment). Scoped the same way too: only
         * touches matchedTarget's RCB if its CURRENT live DatSet actually is
         * this candidate, never blind - handled internally by the helper. */
        if (disableUnbindAndDeleteDataset(handle, matchedTarget->objectReference, candidate)) {
            fprintf(stderr, "[mms_report_client] cleaned up orphaned dataset '%s' - not needed by any "
                    "target this cycle, reclaiming budget\n", candidate);
        } else {
            if (outLeakedCount) (*outLeakedCount)++;
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
     * DynamicDatasetSession's own doc comment. Two independent pools - see
     * that same doc comment for why only Conf gets a discovery correction.
     * Computed as plain locals, BEFORE the session initializer below, rather
     * than inline within it (as this used to do for chunkAssignments) - C
     * gives no evaluation-order guarantee between two initializer
     * expressions in the same struct literal, and buildWholeDeviceClusterPlan
     * below now genuinely needs both budgets already computed (its own
     * explicitClusterBudget argument), not just DynamicDatasetSession's own
     * fields - relying on coincidental evaluation order here would be fragile
     * either way. */
    int sclDynMax = IedModel_getDynDataSetMax(handle->iedModel);
    int sclConfMax = IedModel_getConfDataSetMax(handle->iedModel);
    int existingCount = LinkedList_size(existingServerDatasets);
    int dynBudget = MmsReportClientUseCases_computeInitialDynamicDatasetBudget(sclDynMax, 0);
    int confBudget = MmsReportClientUseCases_computeInitialDynamicDatasetBudget(sclConfMax, existingCount);
    /* Upper bound only, not the real gate - see buildWholeDeviceClusterPlan's
     * own doc comment. -1 (either pool uncapped) must stay uncapped here too,
     * never accidentally become a real 0/negative cap via arithmetic. */
    int explicitClusterBudget = (dynBudget < 0 || confBudget < 0) ? -1 : dynBudget + confBudget;

    DynamicDatasetSession session = {
        .cache = LinkedList_create(),
        .remainingDynBudget = dynBudget,
        .dynBudgetExhaustedLogged = false,
        .remainingConfBudget = confBudget,
        .confBudgetExhaustedLogged = false,
        .chunkAssignments = buildWholeDeviceClusterPlan(handle, explicitClusterBudget),
        .existingServerDatasets = existingServerDatasets,
        .claimedDatasetNames = LinkedList_create(),
    };
    /* One-line, always-on summary of this cycle's real dataset budgets -
     * previously only inferable indirectly, either from the one-shot "budget
     * exhausted" line (which never fires at all if targets simply run out of
     * chunk assignments before the budget itself runs out, see
     * getOrCreateDynamicDataset's own doc comment) or by manually counting
     * per-RCB "resolved via none"/error 99 lines across a whole log capture -
     * exactly the reconstruction this feature's own real-device
     * investigations have had to do by hand. */
    fprintf(stderr, "[mms_report_client] DynDataSet budget this cycle: SCL declares max=%d, starting budget=%d "
            "(uncorrected - not discoverable ahead of time)\n", sclDynMax, session.remainingDynBudget);
    fprintf(stderr, "[mms_report_client] ConfDataSet budget this cycle: SCL declares max=%d, %d already exist "
            "on server, starting budget=%d\n", sclConfMax, existingCount, session.remainingConfBudget);

    LinkedList element = LinkedList_getNext(handle->targets);
    while (element && !handle->stopRequested) {
        enableOneTarget(handle, (ReportControlBlockTarget*) LinkedList_getData(element), &session);
        element = LinkedList_getNext(element);
    }

    int leakedThisCycle = 0;
    if (!handle->stopRequested) cleanupOrphanedDatasets(handle, &session, &leakedThisCycle);
    if (leakedThisCycle > 0) {
        fprintf(stderr, "[mms_report_client] %d domain-scoped dataset(s) could not be reclaimed this cycle "
                "(server denied deletion) - permanently spent against this device's dataset quota until a "
                "device-side reset\n", leakedThisCycle);
    }

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
     * dataset-delete loop below and BEFORE IedConnection_close (both
     * setRCBValues and deleteDataSet need a live association). */
    if (handle->connection && handle->domainScopedDynamicDatasetNames && handle->targets) {
        int trackedCount = LinkedList_size(handle->domainScopedDynamicDatasetNames);
        int targetCount = LinkedList_size(handle->targets);
        int matchedCount = 0;
        int unbindFailCount = 0;
        fprintf(stderr, "[mms_report_client] stop: disabling+unbinding before delete - %d domain-scoped "
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
                char* liveDataSet = liveDataSetLive ? MmsReportClientUtils_safeStringDup(liveDataSetLive) : NULL;
                if (liveDataSet && stringListContains(handle->domainScopedDynamicDatasetNames, liveDataSet)) {
                    matchedCount++;
                    ClientReportControlBlock_setRptEna(rcb, false);
                    ClientReportControlBlock_setDataSetReference(rcb, "");
                    IedConnection_setRCBValues(handle->connection, &err, rcb,
                            RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_DATSET, true);
                    if (err != IED_ERROR_OK) {
                        unbindFailCount++;
                        fprintf(stderr, "[mms_report_client] could not disable/unbind '%s' (dataset '%s') "
                                "before dataset cleanup: error %d\n", target->objectReference, liveDataSet, err);
                    } else {
                        fprintf(stderr, "[mms_report_client] disabled/unbound '%s' from dataset '%s'\n",
                                target->objectReference, liveDataSet);
                    }
                }
                free(liveDataSet);
                ClientReportControlBlock_destroy(rcb);
            }
            element = LinkedList_getNext(element);
        }
        fprintf(stderr, "[mms_report_client] stop: disable+unbind pass done - %d target(s) matched a "
                "tracked dataset, %d succeeded, %d failed\n", matchedCount, matchedCount - unbindFailCount,
                unbindFailCount);
    }

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
        int toDelete = LinkedList_size(handle->domainScopedDynamicDatasetNames);
        int deletedOnStop = 0;
        int leakedOnStop = 0;
        fprintf(stderr, "[mms_report_client] stop: attempting to delete %d domain-scoped dataset(s)\n", toDelete);
        LinkedList element = LinkedList_getNext(handle->domainScopedDynamicDatasetNames);
        while (element) {
            char* datasetName = (char*) LinkedList_getData(element);
            IedClientError deleteErr = IED_ERROR_OK;
            if (!IedConnection_deleteDataSet(handle->connection, &deleteErr, datasetName)) {
                fprintf(stderr, "[mms_report_client] could not delete dynamic dataset '%s' on stop: "
                        "error %d - left behind on the device\n", datasetName, deleteErr);
                leakedOnStop++;
            } else {
                fprintf(stderr, "[mms_report_client] deleted dynamic dataset '%s' on stop\n", datasetName);
                deletedOnStop++;
            }
            element = LinkedList_getNext(element);
        }
        fprintf(stderr, "[mms_report_client] stop: dataset deletion done - %d of %d succeeded, %d left "
                "behind\n", deletedOnStop, toDelete, leakedOnStop);
        if (leakedOnStop > 0) {
            fprintf(stderr, "[mms_report_client] %d domain-scoped dataset(s) could not be deleted on stop - "
                    "permanently spent against this device's dataset quota until a device-side reset\n",
                    leakedOnStop);
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
