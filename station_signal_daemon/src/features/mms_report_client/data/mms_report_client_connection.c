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
    bool associationSpecificCreateRejected; /* Latched the first time an association-specific
                                     ("@"-prefixed) createDataSet fails and the domain-scoped
                                     fallback then succeeds - see createAndCacheDynamicDataset.
                                     Some real devices (confirmed: SIPROTEC 6MD) reject the
                                     association-specific FORM outright, every time, independent
                                     of quota; without this latch every unbuffered Dyn target
                                     spends one doomed ~100-member createDataSet round-trip before
                                     falling back, which on a real device was nine wasted large
                                     PDUs per connect cycle. Per-cycle only, never persisted, so a
                                     device that starts accepting them is retried on the next
                                     connect. */
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
        /* Logged on every derivation, not just on use: this name is the join
         * key for reuse-on-reconnect (IED_ERROR_OBJECT_EXISTS), for
         * adoptUnclaimedDataset's own-name preference, for
         * cleanupOrphanedDatasets' strict match, and for the stop-path
         * deletion pass. A single character's difference between what this
         * builds and what the server actually holds silently breaks all four,
         * and was previously invisible in a log capture. */
        /* Says "domain-scoped", not "buffered RCB": this function's `buffered`
         * parameter selects the SCOPE, and createAndCacheDynamicDataset passes
         * true for it on an UNBUFFERED target's domain-scoped fallback too. The
         * old wording asserted the RCB was buffered and was flatly wrong for
         * every fallback - actively confusing while reading these logs to
         * diagnose exactly that path. */
        fprintf(stderr, "[mms_report_client] derived domain-scoped dataset name '%s' from '%s' (persists past "
                "this connection, '.' folded to '$' to match the server's own wire form)\n",
                name, lnReference);
        return name;
    }

    size_t len = strlen("@dyn_") + strlen(lnReference) + 1;
    char* name = malloc(len);
    if (!name) return NULL;
    snprintf(name, len, "@dyn_%s", lnReference);
    for (char* p = name; *p; p++) {
        if (*p == '/') *p = '_';
    }
    fprintf(stderr, "[mms_report_client] derived association-scoped dataset name '%s' from '%s' (destroyed "
            "automatically when this connection closes, no cleanup tracking needed)\n",
            name, lnReference);
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
        if (strcmp((char*) LinkedList_getData(element), datasetName) == 0) {
            fprintf(stderr, "[mms_report_client] domain-scoped dataset '%s' already tracked for cleanup - "
                    "not re-added\n", datasetName);
            return;
        }
        element = LinkedList_getNext(element);
    }

    LinkedList_add(handle->domainScopedDynamicDatasetNames, MmsReportClientUtils_safeStringDup(datasetName));
    /* This list is the ONLY record of what the graceful stop path has to
     * delete - a name that never lands here permanently consumes the
     * device's dataset quota if the daemon is later killed. Logging the
     * insert makes that bookkeeping auditable from a log capture rather than
     * only observable as quota that mysteriously never comes back. */
    fprintf(stderr, "[mms_report_client] tracking domain-scoped dataset '%s' for cleanup on stop (%d name(s) "
            "tracked so far)\n", datasetName, LinkedList_size(handle->domainScopedDynamicDatasetNames));
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
                fprintf(stderr, "[mms_report_client] could not delete diagnostic dataset '%s': %s (%d) - left "
                        "behind on the device\n", datasetName, IedClientError_toString(deleteErr), deleteErr);
            }
        }
    }

    LinkedList_destroyDeep(specs, (LinkedListValueDeleteFunction) MmsVariableAccessSpecification_destroy);
}

static const char*
createAndCacheDynamicDatasetAttempt(MmsReportClientHandle handle, DynamicDatasetSession* session, const char* cacheKey,
        const char* logLnReference, const char* logRcbReference, const char* const* memberReferences,
        int memberCount, bool buffered) {
    /* Which pool this attempt draws from is exactly what `buffered` already
     * means here - see DynamicDatasetSession's own doc comment. Checked
     * before any wire work so an already-exhausted pool costs nothing beyond
     * this one log line (edge-triggered, see that same doc comment). */
    int* budget = buffered ? &session->remainingConfBudget : &session->remainingDynBudget;
    bool* budgetExhaustedLogged = buffered ? &session->confBudgetExhaustedLogged : &session->dynBudgetExhaustedLogged;

    /* Entry banner. Every input this attempt depends on, in one place, before
     * anything can fail - so a rejection further down can be read against the
     * exact request that produced it rather than reconstructed from
     * surrounding lines. */
    fprintf(stderr, "[mms_report_client] === createDataSet attempt for '%s' (LN '%s') === scope=%s, "
            "requested members=%d, pool=%s, remaining budget=%d\n",
            logRcbReference, logLnReference, buffered ? "domain-scoped" : "association-specific",
            memberCount, buffered ? "ConfDataSet" : "DynDataSet", *budget);

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

    /* Every member reference this attempt was ASKED to include, before any
     * conversion - the input side of the comparison against the wire form
     * below. Verbose on purpose: on a device rejecting a create for reasons
     * it won't name (IED_ERROR_UNKNOWN/99), the exact member list is the only
     * thing left to reason about. */
    for (int i = 0; i < memberCount; i++) {
        fprintf(stderr, "[mms_report_client]   requested member[%d/%d] for '%s': %s\n",
                i + 1, memberCount, logRcbReference, memberReferences[i] ? memberReferences[i] : "(null)");
    }

    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(memberReferences, memberCount);
    if (!wireRefs || LinkedList_size(wireRefs) == 0) {
        fprintf(stderr, "[mms_report_client] no wire-convertible attribute references for LN '%s' (0 of %d "
                "converted) - '%s' will not get a dynamic dataset\n", logLnReference, memberCount,
                logRcbReference);
        if (wireRefs) LinkedList_destroyDeep(wireRefs, free);
        return NULL;
    }

    /* A PARTIAL conversion loss silently changes the dataset's shape - the
     * created dataset genuinely has fewer members than this target's cluster
     * assignment believes, which then has to be reconciled by
     * ensureLnFallbackMemberRefCache against a member list that no longer
     * matches. Previously only the total-loss case above said anything at
     * all, so a partial drop left no trace whatsoever. */
    int wireCount = LinkedList_size(wireRefs);
    if (wireCount != memberCount) {
        fprintf(stderr, "[mms_report_client] WARNING: converted only %d of %d member reference(s) to wire form "
                "for '%s' - %d dropped; the created dataset's real shape will differ from this target's own "
                "cluster assignment\n", wireCount, memberCount, logRcbReference, memberCount - wireCount);
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
    fprintf(stderr, "[mms_report_client] creating dynamic dataset '%s' for LN '%s' with %d member attribute(s) "
            "(as actually sent on the wire)\n", datasetName, logLnReference, wireCount);
    int wireIndex = 0;
    for (LinkedList wireElement = LinkedList_getNext(wireRefs); wireElement;
            wireElement = LinkedList_getNext(wireElement)) {
        wireIndex++;
        fprintf(stderr, "[mms_report_client]   wire member[%d/%d] of '%s': %s\n",
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
     * time (every FC=ST/MX leaf under this LN, or this target's own chunk),
     * so an existing dataset by this name is presumed to already have the
     * right shape. */
    bool reusedExisting = false;
    if (err == IED_ERROR_OBJECT_EXISTS && buffered) {
        fprintf(stderr, "[mms_report_client] dynamic dataset '%s' for LN '%s' already exists on the server - "
                "reusing it (budget unchanged at %d, it was never new this cycle)\n",
                datasetName, logLnReference, *budget);
        reusedExisting = true;
    } else if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] dynamic dataset creation FAILED for LN '%s' (dataset '%s', %d "
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
         * count this cycle (see DynamicDatasetSession's own doc comment on
         * why cache hits stay free; this is the same reasoning applied to a
         * dataset the SERVER already knew about before this cycle started). */
        if (*budget > 0) (*budget)--;
        fprintf(stderr, "[mms_report_client] created dynamic dataset '%s' for LN '%s' with %d member(s) - "
                "%s budget now %d\n", datasetName, logLnReference, wireCount,
                buffered ? "ConfDataSet" : "DynDataSet", *budget);
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
 * "more scoped" fallback left to try.
 *
 * Once that fallback has actually worked once this cycle, the association-
 * specific attempt is skipped for every remaining unbuffered target
 * (session->associationSpecificCreateRejected). On a device that rejects the
 * form structurally it never succeeds for ANY target, so retrying it per
 * target only spends a doomed ~100-member createDataSet round-trip each time -
 * measured at nine wasted large PDUs in one real connect cycle, against a
 * device this feature already works hard not to hammer. The latch is per
 * connect cycle and never persisted, so nothing permanently gives up on the
 * cheaper association-specific form. */
static const char*
createAndCacheDynamicDataset(MmsReportClientHandle handle, DynamicDatasetSession* session, const char* cacheKey,
        const char* logLnReference, const char* logRcbReference, const char* const* memberReferences,
        int memberCount, bool buffered) {
    if (!buffered && session->associationSpecificCreateRejected) {
        fprintf(stderr, "[mms_report_client] skipping the association-specific createDataSet attempt for LN "
                "'%s' - this device already rejected that form earlier this cycle, so going straight to a "
                "domain-scoped dataset instead of spending another doomed %d-member round-trip\n",
                logLnReference, memberCount);
        return createAndCacheDynamicDatasetAttempt(handle, session, cacheKey, logLnReference, logRcbReference,
                memberReferences, memberCount, true);
    }

    const char* result = createAndCacheDynamicDatasetAttempt(handle, session, cacheKey, logLnReference,
            logRcbReference, memberReferences, memberCount, buffered);
    if (result || buffered) return result;

    fprintf(stderr, "[mms_report_client] association-specific dataset creation failed for LN '%s' - falling "
            "back to a domain-scoped dataset instead (some real devices reject association-specific creation "
            "outright, independent of quota - see CHANGELOG.md) - this dataset will NOT be cleaned up "
            "automatically and permanently consumes device quota until a device-side reset\n", logLnReference);
    const char* fallback = createAndCacheDynamicDatasetAttempt(handle, session, cacheKey, logLnReference,
            logRcbReference, memberReferences, memberCount, true);

    /* Latch only on a fallback that actually WORKED. If the domain-scoped
     * attempt failed too, the association-specific form isn't demonstrably the
     * problem (both pools may simply be exhausted), so nothing is concluded and
     * the next target retries normally. */
    if (fallback && !session->associationSpecificCreateRejected) {
        session->associationSpecificCreateRejected = true;
        fprintf(stderr, "[mms_report_client] this device rejects association-specific createDataSet but "
                "accepts domain-scoped - remaining unbuffered targets this cycle will skip the "
                "association-specific attempt entirely\n");
    }
    return fallback;
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
 * etc.).
 *
 * *outWasNeeded distinguishes those NULL cases from each other, and is the
 * ONLY signal that does - they are otherwise indistinguishable to the caller,
 * which is exactly the conflation this out-param exists to break:
 *
 *   false => this target was NEVER NEEDED this cycle. Clustering simply ran
 *            out of clusters before it ran out of Dyn RCB slots, i.e. the
 *            device's reportable data is already fully covered by other
 *            targets. A benign, expected outcome on any device carrying
 *            redundant spare RCB instances (a real SIPROTEC has dozens), not
 *            a failure, and enableOneTarget must not report it as one.
 *   true  => a dataset genuinely WAS needed here and could not be obtained
 *            (budget exhausted, createDataSet rejected, no wire-convertible
 *            members, or a malformed model with no LN reference). This RCB's
 *            share of the device's data will go unreported - a real problem.
 *
 * Always written before any return, including the success paths (where it is
 * false: nothing is outstanding once a dataset resolved).
 */
static const char*
getOrCreateDynamicDataset(MmsReportClientHandle handle, ReportControlBlockTarget* target,
        DynamicDatasetSession* session, bool* outWasNeeded) {
    *outWasNeeded = false;

    if (!session || !target->lnReference) {
        /* A target with no LN reference is a malformed model, not a spare
         * slot the device happens not to need - genuinely a failure. */
        *outWasNeeded = true;
        fprintf(stderr, "[mms_report_client] tier 4 (self-create) unavailable for '%s': %s\n",
                target->objectReference, session ? "target has no LN reference" : "no dataset session");
        return NULL;
    }

    DynamicDatasetChunkAssignment* chunk =
            lookupChunkAssignment(session->chunkAssignments, target->objectReference);
    if (!chunk) {
        /* Not an error - whole-device clustering ran out of clusters before
         * it ran out of Dyn RCB slots, i.e. the device's reportable data is
         * already fully covered by other targets this cycle (see
         * buildWholeDeviceClusterPlan's own underflow log). Stated explicitly
         * so it reads as "nothing left to cover" instead of looking like a
         * lookup bug, and reported via *outWasNeeded=false so the caller
         * leaves this RCB alone entirely rather than calling it failed. */
        fprintf(stderr, "[mms_report_client] tier 4 (self-create) skipped for '%s': no whole-device cluster was "
                "assigned to this RCB slot this cycle (the device's reportable data is already fully covered "
                "by other slots)\n", target->objectReference);
        return NULL;
    }
    fprintf(stderr, "[mms_report_client] tier 4 (self-create) for '%s': assigned cluster has %d member(s)\n",
            target->objectReference, chunk->memberCount);

    const char* existing = lookupDynamicDatasetName(session->cache, target->objectReference, target->buffered);
    if (existing) {
        fprintf(stderr, "[mms_report_client] tier 4 (self-create) for '%s': session cache hit - reusing '%s' "
                "already created this cycle, no wire call\n", target->objectReference, existing);
        return existing;
    }

    /* Cache hits above stay free (zero wire cost, always allowed) - the
     * budget check itself now lives in createAndCacheDynamicDatasetAttempt,
     * keyed per-attempt on which pool that attempt actually draws from (see
     * DynamicDatasetSession's own doc comment) - a single top-level check
     * here couldn't distinguish an unbuffered target's Dyn-budget-gated real
     * attempt from its own Conf-budget-gated fallback. */
    const char* created = createAndCacheDynamicDataset(handle, session, target->objectReference,
            target->lnReference, target->objectReference, (const char* const*) chunk->memberReferences,
            chunk->memberCount, target->buffered);
    /* Past the chunk lookup above, this target demonstrably HAD work to do,
     * so a NULL here is a genuine failure to do it - never the benign
     * "nothing left to cover" case. */
    if (!created) *outWasNeeded = true;
    return created;
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
    /* Both of these rejections used to return silently, which made tier 2
     * indistinguishable from tier 2 never having been consulted at all - the
     * log jumped straight from "resolving" to whatever tier 3/4 did next. */
    if (!liveDataset || liveDataset[0] == '\0') {
        fprintf(stderr, "[mms_report_client] tier 2 (pull live) for '%s': device has no DatSet assigned to this "
                "RCB right now - falling through\n", target->objectReference);
        return false;
    }
    fprintf(stderr, "[mms_report_client] tier 2 (pull live) for '%s': device reports DatSet='%s'\n",
            target->objectReference, liveDataset);
    if (looksLikeOurOwnDynamicDatasetName(liveDataset, target)) {
        fprintf(stderr, "[mms_report_client] tier 2 (pull live) for '%s': '%s' is this client's own "
                "association-scoped name from a PRIOR connection and is already destroyed server-side - "
                "dangling, falling through\n", target->objectReference, liveDataset);
        return false;
    }

    IedClientError err = IED_ERROR_OK;
    bool isDeletable = false;
    LinkedList acsiMembers = IedConnection_getDataSetDirectory(handle->connection, &err, liveDataset, &isDeletable);
    if (!acsiMembers || LinkedList_size(acsiMembers) == 0) {
        fprintf(stderr, "[mms_report_client] live-assigned dataset '%s' for '%s' could not be resolved: %s (%d) "
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
    /* The real decode shape every report on this RCB will be interpreted
     * against (refreshPulledMemberRefCache below rebuilds the member cache
     * from exactly this list) - a mismatch between it and what the device
     * actually sends is the root cause class behind silently misattributed
     * values, and was previously only ever visible as a member COUNT. */
    int pulledIndex = 0;
    int pulledCount = LinkedList_size(wireRefs);
    for (LinkedList pulledElement = LinkedList_getNext(wireRefs); pulledElement;
            pulledElement = LinkedList_getNext(pulledElement)) {
        pulledIndex++;
        fprintf(stderr, "[mms_report_client]   pulled member[%d/%d] of '%s': %s\n",
                pulledIndex, pulledCount, liveDataset, (char*) LinkedList_getData(pulledElement));
    }

    if (session) {
        LinkedList_add(session->claimedDatasetNames, MmsReportClientUtils_safeStringDup(liveDataset));
    }

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
    char* previousDatasetReference = (needsRebuild && entry->resolvedDatasetReference)
            ? MmsReportClientUtils_safeStringDup(entry->resolvedDatasetReference) : NULL;
    Semaphore_post(handle->memberRefCacheLock);
    if (!needsRebuild) return;

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
        if (!mmsNames) {
            /* Previously a silent continue - an LD whose directory query is
             * refused contributes nothing to either the budget correction or
             * the adoption pool, which then looks identical to an LD that
             * genuinely holds no datasets at all. */
            fprintf(stderr, "[mms_report_client] could not list datasets under LD '%s': %s (%d) - this LD "
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
            fprintf(stderr, "[mms_report_client]   existing dataset on server: '%s'\n", full);
        }
        fprintf(stderr, "[mms_report_client] LD '%s' holds %d existing dataset(s)\n", ldName, perLdCount);
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
            fprintf(stderr, "[mms_report_client] excluding '%s' from the adoption pool - our own model says it "
                    "is a DIFFERENT RCB's SCL-declared static datSet, so adopting it would duplicate coverage "
                    "instead of adding any\n", candidate);
            LinkedList_remove(existing, candidate);
            free(candidate);
        }
    }

    fprintf(stderr, "[mms_report_client] discovery complete: %d existing dataset(s) already on the server "
            "across this client's own LD(s), available as adoption candidates and as the ConfDataSet budget "
            "correction\n", LinkedList_size(existing));

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
    fprintf(stderr, "[mms_report_client] tier 3 (adopt) for '%s': considering candidate '%s'\n",
            target->objectReference, candidate);

    IedClientError err = IED_ERROR_OK;
    bool isDeletable = false;
    LinkedList acsiMembers = IedConnection_getDataSetDirectory(handle->connection, &err, candidate, &isDeletable);
    if (!acsiMembers || LinkedList_size(acsiMembers) == 0) {
        fprintf(stderr, "[mms_report_client] discovered dataset '%s' could not be resolved: %s (%d) - "
                "skipping as an adoption candidate\n", candidate, IedClientError_toString(err), err);
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

    fprintf(stderr, "[mms_report_client] adopting existing dataset '%s' for '%s' (%d member(s), deletable=%d) - "
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
        fprintf(stderr, "[mms_report_client]   adopted member[%d/%d] of '%s': %s\n",
                adoptedIndex, adoptedCount, candidate, (char*) LinkedList_getData(adoptedElement));
    }

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
    if (!slash) {
        fprintf(stderr, "[mms_report_client] tier 3 (adopt) unavailable for '%s': LN reference '%s' has no '/', "
                "so its LD cannot be determined\n", target->objectReference, target->lnReference);
        return NULL;
    }
    size_t ldLen = (size_t) (slash - target->lnReference);

    if (target->buffered && target->objectReference) {
        char* ownName = buildDynamicDatasetName(target->objectReference, true);
        if (ownName) {
            LinkedList element = LinkedList_getNext(session->existingServerDatasets);
            while (element) {
                char* candidate = (char*) LinkedList_getData(element);
                element = LinkedList_getNext(element);
                if (strcmp(candidate, ownName) != 0) continue;
                if (stringListContains(session->claimedDatasetNames, candidate)) {
                    fprintf(stderr, "[mms_report_client] tier 3 (adopt) for '%s': its own previous name '%s' is "
                            "on the server but was already claimed this cycle - falling through to the "
                            "LD-wide scan\n", target->objectReference, candidate);
                    break;
                }
                fprintf(stderr, "[mms_report_client] tier 3 (adopt) for '%s': found its OWN previous dataset "
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
        if (stringListContains(session->claimedDatasetNames, candidate)) {
            fprintf(stderr, "[mms_report_client] tier 3 (adopt) for '%s': candidate '%s' already claimed by "
                    "another target this cycle - skipping\n", target->objectReference, candidate);
            continue;
        }

        if (tryAdoptCandidate(handle, target, session, candidate, outMemberRefs)) return candidate;
    }

    fprintf(stderr, "[mms_report_client] tier 3 (adopt) found nothing usable for '%s': %d candidate(s) exist "
            "under its LD, all already claimed or unusable - falling through to self-create\n",
            target->objectReference, consideredCandidates);
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

/* ---- Sequential, verified RCB enable-sequence plumbing --------------------
 *
 * IEC 61850-7-2 makes an RCB's configuration attributes (DatSet, OptFlds,
 * TrgOps, BufTm, IntgPd) writable only while RptEna is FALSE. This feature
 * previously asked for every one of them PLUS RptEna=true inside a SINGLE
 * bundled IedConnection_setRCBValues call. Two things went wrong with that
 * against real hardware:
 *
 *   - A rejection named the whole REQUEST, never the ELEMENT. One error code
 *     covered "the device didn't like the dataset", "...the trigger options",
 *     "...the EntryID" and "...the enable itself" indistinguishably, so a log
 *     capture from a real device could never say which attribute the device
 *     actually objected to - the single biggest obstacle to diagnosing why
 *     RCBs weren't going active on a real SIPROTEC.
 *   - On a reconnect where the device still reports the RCB as already
 *     enabled, every config element in that bundle is illegal per the rule
 *     above, and a bundle has no way to disable first.
 *
 * The enable is therefore now a strict, spec-ordered sequence of SINGLE-element
 * writes (see enableOneTarget's own step list), each one run through
 * runRcbStep below, which logs what it intended, what the device returned (by
 * NAME, via IedClientError_toString, not just a bare code), and what the
 * device itself reports on an independent read-back immediately afterwards.
 *
 * NOTE ON LOG VOLUME: this block is deliberately verbose - up to six writes
 * and seven extra reads per RCB per connect cycle, each with its own log
 * lines. That is the point (a device that accepts a write and silently never
 * applies it is structurally invisible any other way), but it is a
 * DIAGNOSTIC posture, not a steady-state one - see CHANGELOG.md.
 */

/* One snapshot of an RCB's live, device-reported state. `datSet` is an OWNED
 * copy, not the borrowed pointer ClientReportControlBlock_getDataSetReference
 * hands back - that one points into the RCB object's own internal buffer and
 * dies with ClientReportControlBlock_destroy (and is overwritten in place by
 * a subsequent setDataSetReference, the exact bug fixed on the stop path in
 * this file's history). Always pair with destroyRcbLiveState. */
typedef struct {
    bool readOk;
    bool rptEna;
    char* datSet;
    int optFlds;
    int trgOps;
    uint32_t bufTm;
    uint32_t intgPd;
    uint32_t confRev;
    uint16_t sqNum;
    bool hasEntryId;
} RcbLiveState;

static void
destroyRcbLiveState(RcbLiveState* state) {
    if (!state) return;
    free(state->datSet);
    state->datSet = NULL;
}

/* Snapshots an already-fetched RCB object's device-reported values into an
 * RcbLiveState and logs them as one line. Split out from
 * readAndLogLiveRcbState so enableOneTarget's own entry log can reuse the
 * getRCBValues result it already has, rather than paying a second, identical
 * round-trip purely to print the same numbers. */
static void
captureAndLogRcbState(const char* objectReference, const char* when, ClientReportControlBlock rcb,
        RcbLiveState* out) {
    const char* datasetReference = ClientReportControlBlock_getDataSetReference(rcb);
    out->readOk = true;
    out->rptEna = ClientReportControlBlock_getRptEna(rcb);
    out->datSet = datasetReference ? MmsReportClientUtils_safeStringDup(datasetReference) : NULL;
    out->optFlds = ClientReportControlBlock_getOptFlds(rcb);
    out->trgOps = ClientReportControlBlock_getTrgOps(rcb);
    out->bufTm = ClientReportControlBlock_getBufTm(rcb);
    out->intgPd = ClientReportControlBlock_getIntgPd(rcb);
    out->confRev = ClientReportControlBlock_getConfRev(rcb);
    out->sqNum = ClientReportControlBlock_getSqNum(rcb);
    out->hasEntryId = ClientReportControlBlock_getEntryId(rcb) != NULL;

    fprintf(stderr, "[mms_report_client] '%s' live RCB state (%s): RptEna=%d DatSet='%s' OptFlds=0x%x "
            "TrgOps=0x%x BufTm=%u IntgPd=%u ConfRev=%u SqNum=%u EntryID=%s\n",
            objectReference, when, out->rptEna, out->datSet ? out->datSet : "(empty)", out->optFlds,
            out->trgOps, out->bufTm, out->intgPd, out->confRev, (unsigned) out->sqNum,
            out->hasEntryId ? "present" : "absent");
}

/* Reads the RCB straight back off the device as its OWN MMS read - deliberately
 * independent of whatever this client just wrote - and logs every attribute
 * this feature's enable sequence touches. Exists because a setRCBValues call
 * can return IED_ERROR_OK while the device never actually applies the value;
 * without a real read-back there is no way to tell "we wrote it and it stuck"
 * from "we wrote it and the device silently ignored it" from a log capture
 * alone. Best-effort and purely diagnostic: a failed read logs and sets
 * readOk=false, and is never treated as the enable itself failing. */
static void
readAndLogLiveRcbState(MmsReportClientHandle handle, const char* objectReference, const char* when,
        RcbLiveState* out) {
    memset(out, 0, sizeof(*out));

    IedClientError err = IED_ERROR_OK;
    ClientReportControlBlock live = IedConnection_getRCBValues(handle->connection, &err, objectReference, NULL);
    if (!live) {
        fprintf(stderr, "[mms_report_client] '%s' live read-back (%s) FAILED: %s (%d) - device state unknown "
                "at this point\n", objectReference, when, IedClientError_toString(err), err);
        return;
    }

    captureAndLogRcbState(objectReference, when, live, out);
    ClientReportControlBlock_destroy(live);
}

/* What runRcbStep should compare its own read-back against, to decide whether
 * the write it just made actually took effect on the device. */
typedef enum {
    RCB_STEP_VERIFY_NONE,     /* EntryID / GI - write-only in effect, a read-back proves nothing */
    RCB_STEP_VERIFY_DATSET,   /* expectedString, exact match */
    RCB_STEP_VERIFY_OPT_FLDS, /* expectedBits, containment only - see below */
    RCB_STEP_VERIFY_TRG_OPS,  /* expectedBits, containment only - see below */
    RCB_STEP_VERIFY_RPT_ENA,  /* expectedBool */
} RcbStepVerify;

/* One step of the enable sequence. `element` must be EXACTLY ONE RCB_ELEMENT_*
 * constant - runRcbStep's whole reason for existing is that a failure names
 * one attribute, which a multi-element mask would defeat, and its verify
 * logic assumes a single attribute per call. `intendedText` is caller-formatted
 * "what we are asking for", for the log only. */
typedef struct {
    uint32_t element;
    const char* name;
    const char* intendedText;
    RcbStepVerify verify;
    const char* expectedString;
    int expectedBits;
    bool expectedBool;
} RcbStep;

/* Writes one element of an RCB, then proves (or disproves) that it landed.
 *
 * Bit-valued verifies check CONTAINMENT ((live & expected) == expected), never
 * equality: this feature only ever ORs bits into OptFlds/TrgOps and never
 * clobbers bits the device already had (see enableOneTarget's own doc comments
 * on both), so a live value carrying extra bits is correct, not a mismatch.
 *
 * The TEMPORARILY_UNAVAILABLE retry loop that used to wrap the single bundled
 * write now lives here, per-step - a device still initializing after a restart
 * can return it for any element, not just whichever one happened to be last.
 *
 * Returns the setRCBValues error. *outApplied reports whether the read-back
 * actually confirmed the value (true for RCB_STEP_VERIFY_NONE, which has
 * nothing to confirm; false whenever the read-back failed or contradicted the
 * write) - deliberately separate from the return value, because "device said
 * OK but didn't do it" is exactly the failure mode that has no error code.
 *
 * *outLive (optional) hands the caller this step's own read-back instead of
 * discarding it, so a LATER step's decision can be based on what the device
 * reports NOW rather than on a snapshot taken before this write. That is not a
 * convenience: a real SIPROTEC clears TrgOps/OptFlds whenever DatSet changes,
 * so enableOneTarget's steps 2 and 3 MUST judge against the state left behind
 * by step 1, not against its own entry snapshot (see this file's CHANGELOG
 * entry - deciding from the stale snapshot silently enabled RCBs with
 * TrgOps=0, which can never report anything at all). Ownership transfers to
 * the caller, which must call destroyRcbLiveState; pass NULL to have it
 * destroyed here as usual. */
static IedClientError
runRcbStep(MmsReportClientHandle handle, ClientReportControlBlock rcb, const char* objectReference,
        const RcbStep* step, bool* outApplied, RcbLiveState* outLive) {
    if (outApplied) *outApplied = false;
    if (outLive) memset(outLive, 0, sizeof(*outLive));

    fprintf(stderr, "[mms_report_client] '%s' step '%s': writing %s (element 0x%x)\n",
            objectReference, step->name, step->intendedText, step->element);

    IedClientError err = IED_ERROR_OK;
    IedConnection_setRCBValues(handle->connection, &err, rcb, step->element, true);

    int tempUnavailableRetries = 0;
    while (err == IED_ERROR_TEMPORARILY_UNAVAILABLE
            && tempUnavailableRetries < MMS_REPORT_CLIENT_TEMP_UNAVAILABLE_MAX_RETRIES
            && !handle->stopRequested) {
        tempUnavailableRetries++;
        fprintf(stderr, "[mms_report_client] '%s' step '%s': temporarily unavailable (device likely still "
                "initializing after a restart) - retry %d/%d\n", objectReference, step->name,
                tempUnavailableRetries, MMS_REPORT_CLIENT_TEMP_UNAVAILABLE_MAX_RETRIES);
        interruptibleSleep(handle, MMS_REPORT_CLIENT_TEMP_UNAVAILABLE_RETRY_DELAY_MS);
        IedConnection_setRCBValues(handle->connection, &err, rcb, step->element, true);
    }

    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] '%s' step '%s': setRCBValues FAILED: %s (%d)\n",
                objectReference, step->name, IedClientError_toString(err), err);
    } else {
        fprintf(stderr, "[mms_report_client] '%s' step '%s': setRCBValues returned OK\n",
                objectReference, step->name);
    }

    char when[128];
    snprintf(when, sizeof(when), "after step '%s'", step->name);
    RcbLiveState live;
    readAndLogLiveRcbState(handle, objectReference, when, &live);

    if (step->verify == RCB_STEP_VERIFY_NONE) {
        fprintf(stderr, "[mms_report_client] '%s' step '%s': unverifiable (write-only attribute, nothing to "
                "read back)\n", objectReference, step->name);
        if (outApplied) *outApplied = true;
    } else if (!live.readOk) {
        fprintf(stderr, "[mms_report_client] '%s' step '%s': unverifiable (read-back failed)\n",
                objectReference, step->name);
    } else {
        bool applied = false;
        char actual[256];
        char expected[256];

        switch (step->verify) {
            case RCB_STEP_VERIFY_DATSET:
                applied = live.datSet && step->expectedString && strcmp(live.datSet, step->expectedString) == 0;
                snprintf(actual, sizeof(actual), "DatSet='%s'", live.datSet ? live.datSet : "(empty)");
                snprintf(expected, sizeof(expected), "DatSet='%s'",
                        step->expectedString ? step->expectedString : "(none)");
                break;
            case RCB_STEP_VERIFY_OPT_FLDS:
                applied = (live.optFlds & step->expectedBits) == step->expectedBits;
                snprintf(actual, sizeof(actual), "OptFlds=0x%x", live.optFlds);
                snprintf(expected, sizeof(expected), "OptFlds to contain 0x%x", step->expectedBits);
                break;
            case RCB_STEP_VERIFY_TRG_OPS:
                applied = (live.trgOps & step->expectedBits) == step->expectedBits;
                snprintf(actual, sizeof(actual), "TrgOps=0x%x", live.trgOps);
                snprintf(expected, sizeof(expected), "TrgOps to contain 0x%x", step->expectedBits);
                break;
            case RCB_STEP_VERIFY_RPT_ENA:
                applied = live.rptEna == step->expectedBool;
                snprintf(actual, sizeof(actual), "RptEna=%d", live.rptEna);
                snprintf(expected, sizeof(expected), "RptEna=%d", step->expectedBool);
                break;
            default:
                snprintf(actual, sizeof(actual), "(none)");
                snprintf(expected, sizeof(expected), "(none)");
                break;
        }

        if (applied) {
            fprintf(stderr, "[mms_report_client] '%s' step '%s': VERIFIED - device reports %s\n",
                    objectReference, step->name, actual);
        } else if (err == IED_ERROR_OK) {
            /* The silent-ignore case this whole helper exists to expose. */
            fprintf(stderr, "[mms_report_client] '%s' step '%s': NOT APPLIED - setRCBValues reported success "
                    "but the device's live state is %s, expected %s\n",
                    objectReference, step->name, actual, expected);
        } else {
            fprintf(stderr, "[mms_report_client] '%s' step '%s': not applied (write already failed above) - "
                    "device's live state is %s, expected %s\n",
                    objectReference, step->name, actual, expected);
        }
        if (outApplied) *outApplied = applied;
    }

    if (outLive) {
        *outLive = live; /* ownership of live.datSet transfers to the caller */
    } else {
        destroyRcbLiveState(&live);
    }
    return err;
}

/* How one target's enable attempt ended, for enableAllTargets' own per-cycle
 * tally. The distinction that matters is NOT_NEEDED vs FAILED: both leave the
 * RCB unreported and both used to produce byte-identical output, but only one
 * of them is a problem. On a device carrying dozens of redundant spare RCB
 * instances (a real SIPROTEC does), conflating them buries the handful of real
 * failures among a pile of benign ones. */
typedef enum {
    RCB_ENABLE_OUTCOME_ENABLED,           /* bound and enabled, reporting */
    RCB_ENABLE_OUTCOME_NOT_NEEDED,        /* benign: device's data already fully covered elsewhere */
    RCB_ENABLE_OUTCOME_FAILED,            /* a dataset/enable was genuinely needed and didn't happen */
    RCB_ENABLE_OUTCOME_SKIPPED_STOPPING,  /* a stop landed mid-loop; excluded from the tally */
} RcbEnableOutcome;

static RcbEnableOutcome
enableOneTarget(MmsReportClientHandle handle, ReportControlBlockTarget* target, DynamicDatasetSession* session) {
    /* Defense-in-depth against enableAllTargets' own loop-top check below -
     * covers the narrow gap between that check and this call actually
     * landing, if a stop lands on the connection concurrently mid-loop. */
    if (handle->stopRequested) return RCB_ENABLE_OUTCOME_SKIPPED_STOPPING;

    IedClientError err = IED_ERROR_OK;

    ClientReportControlBlock rcb =
        IedConnection_getRCBValues(handle->connection, &err, target->objectReference, NULL);
    if (!rcb) {
        fprintf(stderr, "[mms_report_client] getRCBValues failed for '%s': %s (%d)\n",
                target->objectReference, IedClientError_toString(err), err);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false, err);
        }
        return RCB_ENABLE_OUTCOME_FAILED;
    }

    /* Everything the device itself says about this RCB before this client
     * touches anything - taken straight from the getRCBValues above, no extra
     * round-trip (see captureAndLogRcbState). Two jobs: it's the baseline
     * every "NOT APPLIED" verdict later in this sequence is judged against,
     * and it supplies the three inputs the steps below branch on (live
     * RptEna - does this RCB need disabling before its config is writable? -
     * plus OptFlds and TrgOps). */
    RcbLiveState entryState;
    memset(&entryState, 0, sizeof(entryState));
    captureAndLogRcbState(target->objectReference, "on entry, before any write", rcb, &entryState);

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

    /* DatSet must be (re-)set explicitly on enable - relying on a
     * server-side default dataset (configured only via ReportControlBlock_create's
     * dataSetName at server build time) is fragile: libiec61850's own
     * reference client example (client_example_no_thread.c) always sets
     * RCB_ELEMENT_DATSET alongside RPT_ENA too, using the same "$"-joined
     * reference format ied_model already hands us in datasetReference.
     *
     * RESOLVED FIRST, BEFORE ANY WIRE WRITE OR ANY LOCAL MUTATION OF `rcb`.
     * A target this client cannot bind a dataset to must not be touched at
     * all - not disabled, not reconfigured, not left half-set-up with a
     * report handler installed for an RCB that will never report. Everything
     * from the report-handler install onwards is therefore gated on this
     * block succeeding.
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
     *
     * The resolved name is kept as an OWNED copy, not the borrowed pointer
     * each tier hands back. Tier 2's own borrowed pointer aliases `rcb`'s
     * internal MmsValue buffer (ClientReportControlBlock_getDataSetReference
     * returns MmsValue_toString of rcb->datSet), and the step-1 write below
     * feeds it straight back into ClientReportControlBlock_setDataSetReference,
     * whose MmsValue_setVisibleString frees and reallocates that exact buffer
     * whenever the new string is longer than the old one - i.e. the argument
     * and the destination are the same allocation. It happens to be benign
     * today (same string, same length, so the realloc branch is never taken),
     * but it is the identical aliasing bug already fixed once on the stop
     * path in this file, and owning the string removes the hazard outright
     * instead of relying on the lengths staying equal. */
    char* effectiveDatasetReference = target->datasetReference
            ? MmsReportClientUtils_safeStringDup(target->datasetReference) : NULL;
    const char* datasetTier = effectiveDatasetReference ? "SCL" : NULL;
    /* Only meaningful if every tier below fails: see getOrCreateDynamicDataset's
     * own doc comment. Distinguishes "this RCB was never needed" from "this
     * RCB needed a dataset and couldn't get one". A target that resolves via
     * tiers 1-3 never reaches tier 4 and leaves this at its false default,
     * which is correct - nothing is outstanding for it either. */
    bool datasetWasNeeded = false;
    if (!effectiveDatasetReference) {
        LinkedList pulledMemberRefs = NULL;
        if (pullLiveDataset(handle, target, rcb, session, &pulledMemberRefs)) {
            const char* liveDataset = ClientReportControlBlock_getDataSetReference(rcb);
            refreshPulledMemberRefCache(handle, target, liveDataset, pulledMemberRefs);
            LinkedList_destroyDeep(pulledMemberRefs, free);
            effectiveDatasetReference = MmsReportClientUtils_safeStringDup(liveDataset);
            datasetTier = "live";
        } else {
            LinkedList adoptedMemberRefs = NULL;
            const char* adopted = adoptUnclaimedDataset(handle, target, session, &adoptedMemberRefs);
            if (adopted) {
                refreshPulledMemberRefCache(handle, target, adopted, adoptedMemberRefs);
                LinkedList_destroyDeep(adoptedMemberRefs, free);
                effectiveDatasetReference = MmsReportClientUtils_safeStringDup(adopted);
                datasetTier = "adopted";
            } else {
                const char* created = getOrCreateDynamicDataset(handle, target, session, &datasetWasNeeded);
                ensureLnFallbackMemberRefCache(handle, target, created, session);
                effectiveDatasetReference = created ? MmsReportClientUtils_safeStringDup(created) : NULL;
                datasetTier = "self-created";
            }
        }
    }
    /* Which of the four tiers (see this function's own doc comment just
     * above) actually resolved the dataset - or that none did, the same
     * "nothing to bind, nothing to enable" case documented there.
     * Logged unconditionally (not just on failure) so a real run shows, per
     * RCB, exactly which path was taken without having to infer it from
     * whichever failure fprintf's did or didn't fire. */
    fprintf(stderr, "[mms_report_client] '%s' dataset resolved via %s: '%s'\n", target->objectReference,
            effectiveDatasetReference ? datasetTier : "none",
            effectiveDatasetReference ? effectiveDatasetReference : "(none)");
    if (!effectiveDatasetReference) {
        /* No tier resolved a dataset, so every write below would be
         * guaranteed to fail (a real device returns
         * IED_ERROR_OBJECT_ATTRIBUTE_INCONSISTENT/31 for RptEna requested with
         * no DatSet bound, confirmed against a real SIPROTEC 6MD device) -
         * skip the doomed sequence rather than making it: against an
         * already-struggling real device, a whole write/read-back sequence per
         * unresolved RCB (potentially 100+ per connect cycle) is pure noise
         * indistinguishable from hammering it, for a result already known
         * here. No report handler has been installed at this point, so there
         * is nothing to uninstall either.
         *
         * TWO GENUINELY DIFFERENT OUTCOMES SHARE THIS BRANCH, and they used to
         * be reported identically - same log line, same synthesized
         * rcbStatusCallback(false, ...) - which on a device with dozens of
         * redundant spare RCB instances buried the handful of real failures
         * under a pile of benign ones. datasetWasNeeded (see
         * getOrCreateDynamicDataset's own doc comment) separates them. */
        if (!datasetWasNeeded) {
            /* Benign: clustering ran out of clusters before it ran out of Dyn
             * RCB slots, so the device's data is already fully covered and
             * this spare slot has nothing left to report on. Deliberately
             * does NOT fire rcbStatusCallback: that callback's own contract
             * (mms_report_client_api.h) is "fires once per RCB after each
             * enable ATTEMPT", and no attempt was made here - the device was
             * never touched. Reporting a non-event as a failure was the bug. */
            fprintf(stderr, "[mms_report_client] '%s' not needed this cycle - the device's reportable data is "
                    "already fully covered by other RCB(s), so this spare slot is deliberately left untouched "
                    "(not a failure)\n", target->objectReference);
            destroyRcbLiveState(&entryState);
            ClientReportControlBlock_destroy(rcb);
            return RCB_ENABLE_OUTCOME_NOT_NEEDED;
        }

        /* Real failure: this target had work to do and couldn't do it. Says
         * so loudly, and points at the tier lines above rather than making
         * the reader correlate by hand. Still fires the callback with the
         * same synthesized error a real attempt would have returned. */
        fprintf(stderr, "[mms_report_client] '%s' FAILED to obtain a dataset - it needed one but none could be "
                "created or adopted for it (see this RCB's own tier 2/3/4 lines above for which step gave up "
                "and why); this RCB will not report\n", target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false,
                    IED_ERROR_OBJECT_ATTRIBUTE_INCONSISTENT);
        }
        destroyRcbLiveState(&entryState);
        ClientReportControlBlock_destroy(rcb);
        return RCB_ENABLE_OUTCOME_FAILED;
    }

    /* Install the handler before enabling, so no report can arrive unhandled
     * in the gap between the two. Deliberately AFTER dataset resolution: a
     * target that never gets a dataset is now abandoned before this point,
     * so there is no longer an install/uninstall churn for RCBs this client
     * was never going to enable. */
    IedConnection_installReportHandler(handle->connection, target->objectReference,
            ClientReportControlBlock_getRptId(rcb), MmsReportClientReportAdapter_onReport, handle);

    /* ---- The enable sequence --------------------------------------------
     *
     * Strict IEC 61850-7-2 order, ONE single-element write per step, each one
     * put through runRcbStep (see its own doc comment, and the block comment
     * above it, for why this is no longer a single bundled write):
     *
     *   0. RptEna=false - only if the device reports it as already enabled.
     *                     DatSet/OptFlds/TrgOps are only writable while
     *                     RptEna is FALSE, so on a reconnect that finds the
     *                     RCB still active, every step below is illegal until
     *                     this one has run.
     *   1. DatSet       - bind the dataset resolved above.            FATAL
     *   2. OptFlds      - buffered only, OR in EntryID.
     *   3. TrgOps       - OR in dchg/qchg/gi.
     *   4. EntryID      - buffered only, the resume point.
     *   5. RptEna=true  - and only now is the RCB actually enabled.    FATAL
     *   6. GI           - a trigger request against an already-active RCB.
     *
     * Only steps 1 and 5 are fatal: without a bound dataset, or without the
     * enable itself, the RCB genuinely cannot report. A failure in step
     * 0/2/3/4/6 is logged loudly and the sequence continues - a degraded RCB
     * that is still bound and enabled beats no RCB at all, and the log then
     * says exactly which capability is degraded and why. */

    /* Step 0. */
    if (entryState.rptEna) {
        fprintf(stderr, "[mms_report_client] '%s' is already enabled on the device - disabling first so its "
                "configuration attributes become writable (IEC 61850-7-2: DatSet/OptFlds/TrgOps/BufTm/IntgPd "
                "are only writable while RptEna is FALSE)\n", target->objectReference);
        ClientReportControlBlock_setRptEna(rcb, false);
        RcbStep disableStep = {
            .element = RCB_ELEMENT_RPT_ENA,
            .name = "0/6 RptEna=false (pre-disable)",
            .intendedText = "RptEna=false",
            .verify = RCB_STEP_VERIFY_RPT_ENA,
            .expectedBool = false,
        };
        /* Non-fatal by design: if the device refuses to disable, the config
         * steps below will each fail on their own and name themselves, which
         * is strictly more diagnostic than bailing out here would be. */
        runRcbStep(handle, rcb, target->objectReference, &disableStep, NULL, NULL);
    } else {
        fprintf(stderr, "[mms_report_client] '%s' step '0/6 RptEna=false (pre-disable)': skipped (device "
                "already reports RptEna=0, configuration attributes are writable as-is)\n",
                target->objectReference);
    }

    /* Step 1. Its own read-back is captured into postDatSetState and becomes
     * the basis for steps 2 and 3 below - see those steps' own comments and
     * runRcbStep's outLive doc comment for why entryState is NOT usable there. */
    RcbLiveState postDatSetState;
    memset(&postDatSetState, 0, sizeof(postDatSetState));
    ClientReportControlBlock_setDataSetReference(rcb, effectiveDatasetReference);
    {
        char intended[512];
        snprintf(intended, sizeof(intended), "DatSet='%s' (resolved via %s)", effectiveDatasetReference,
                datasetTier);
        RcbStep datSetStep = {
            .element = RCB_ELEMENT_DATSET,
            .name = "1/6 DatSet",
            .intendedText = intended,
            .verify = RCB_STEP_VERIFY_DATSET,
            .expectedString = effectiveDatasetReference,
        };
        err = runRcbStep(handle, rcb, target->objectReference, &datSetStep, NULL, &postDatSetState);
    }
    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] '%s' ABORTING enable at step 1 - the dataset binding itself was "
                "rejected, so there is nothing left to enable\n", target->objectReference);
        IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false, err);
        }
        free(effectiveDatasetReference);
        destroyRcbLiveState(&postDatSetState);
        destroyRcbLiveState(&entryState);
        ClientReportControlBlock_destroy(rcb);
        return RCB_ENABLE_OUTCOME_FAILED;
    }

    /* Step 2. Proactively request OptFlds.EntryID for every buffered RCB,
     * rather than relying on however the device happens to already be
     * configured - a real device was found sending ZERO EntryID across every
     * single report (confirmed via a temporary diagnostic log: 2581/2581
     * received reports had no EntryID at all), making the EntryID-resumption
     * mechanism below structurally impossible against it regardless of how
     * correct our own resumption logic is, since there is never anything to
     * cache and resume from. ORs RPT_OPT_ENTRY_ID into whatever OptFlds bits
     * the device already has configured - never clobbers the rest, same
     * minimal-footprint posture as everywhere else here. Only written at all if
     * the bit isn't already set, to avoid touching this attribute on every
     * single reconnect once the device has accepted it once. The alternative (a
     * site-side SCL/engineering-tool config change enabling entryID="true" on
     * the device itself) is noted in CLAUDE.md's own mms_report_client bullet -
     * this client-side approach was chosen instead so the daemon works against
     * a device's default configuration without requiring a site
     * visit/reconfiguration first.
     *
     * JUDGED AGAINST postDatSetState, NOT entryState. A real SIPROTEC clears
     * OptFlds (and TrgOps - see step 3) whenever step 1 writes a DIFFERENT
     * DatSet, so this RCB's OptFlds may well have been wiped microseconds ago
     * and entryState is stale by definition here. Reading the stale snapshot is
     * exactly the bug that silently broke step 3 on real hardware (see this
     * file's CHANGELOG entry): the skip condition passed on a value the device
     * no longer reported. For OptFlds specifically the symptom would be even
     * quieter than step 3's - EntryID resumption would simply stop working,
     * with no error and no missing data, just a full backlog redelivery on
     * every reconnect.
     *
     * A FAILED read-back falls through to writing, never to skipping: if we
     * cannot see the device's state, re-writing a value it may already hold is
     * harmless, whereas skipping risks leaving the attribute wiped. */
    bool optFldsAlreadySet = postDatSetState.readOk && (postDatSetState.optFlds & RPT_OPT_ENTRY_ID);
    if (target->buffered && !optFldsAlreadySet) {
        int deviceOptFlds = postDatSetState.readOk ? postDatSetState.optFlds : 0;
        int desiredOptFlds = deviceOptFlds | RPT_OPT_ENTRY_ID;
        ClientReportControlBlock_setOptFlds(rcb, desiredOptFlds);
        char intended[256];
        snprintf(intended, sizeof(intended), "OptFlds=0x%x (device had 0x%x after the DatSet write%s, OR'ing "
                "in RPT_OPT_ENTRY_ID 0x%x)", desiredOptFlds, deviceOptFlds,
                postDatSetState.readOk ? "" : " - read-back failed, assuming none set", RPT_OPT_ENTRY_ID);
        RcbStep optFldsStep = {
            .element = RCB_ELEMENT_OPT_FLDS,
            .name = "2/6 OptFlds",
            .intendedText = intended,
            .verify = RCB_STEP_VERIFY_OPT_FLDS,
            .expectedBits = RPT_OPT_ENTRY_ID,
        };
        IedClientError optFldsErr = runRcbStep(handle, rcb, target->objectReference, &optFldsStep, NULL, NULL);
        if (optFldsErr != IED_ERROR_OK) {
            fprintf(stderr, "[mms_report_client] '%s' step 2 failed but is NON-FATAL - continuing without "
                    "OptFlds.EntryID; this RCB will report, but EntryID resumption across reconnects will "
                    "not work against this device\n", target->objectReference);
        }
    } else if (!target->buffered) {
        fprintf(stderr, "[mms_report_client] '%s' step '2/6 OptFlds': skipped (unbuffered RCB - EntryID is a "
                "buffered-only concept)\n", target->objectReference);
    } else {
        /* Prints the value actually judged (post-DatSet), never the entry
         * snapshot - a skip justified by a number the device no longer reports
         * is precisely what made the step-3 bug invisible in the last capture. */
        fprintf(stderr, "[mms_report_client] '%s' step '2/6 OptFlds': skipped (device still has "
                "OptFlds.EntryID set after the DatSet write, OptFlds=0x%x)\n",
                target->objectReference, postDatSetState.optFlds);
    }

    /* Step 3. Proactively OR in TrgOps.dchg/qchg/gi for every RCB, rather
     * than relying on however the device happens to already be configured - a
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
     * whatever TrgOps bits the device already has - never clobbers the rest,
     * same minimal-footprint posture as OptFlds
     * above. GI is included here too since this feature's own GI request
     * (step 6 below) depends on TrgOps.gi being enabled server-side to be
     * honored at all, per IEC 61850 - without it, even the bootstrap snapshot
     * this feature already relies on could be silently ignored by a
     * spec-compliant server. Deliberately does NOT touch TRG_OPT_DATA_UPDATE
     * or TRG_OPT_INTEGRITY: integrity is a periodic/timer-based trigger, and
     * this feature is deliberately, strictly event-driven (see CHANGELOG.md) -
     * enabling it would reintroduce exactly the kind of "periodic traffic
     * that looks like an event" problem the value-diff cache exists to filter
     * out, not something worth manufacturing on purpose. Only written at all
     * if at least one of these bits is missing, same reasoning as OptFlds.
     *
     * Written BEFORE RptEna, per this function's own step-order doc comment
     * above - most servers, including this codebase's own `ied_simulator`,
     * only accept a TrgOps change while RptEna is false and reject it with
     * IED_ERROR_TEMPORARILY_UNAVAILABLE once enabled (confirmed directly:
     * integration_tests/mms_report_client's
     * test_dynamicDataset_giOnlyRcb_reportsRealChangeAfterTrgOpsFix fails
     * with exactly that error if this write is deferred to after step 5).
     *
     * JUDGED AGAINST postDatSetState, NOT entryState - THIS IS THE BUG THIS
     * STEP ONCE HAD. A real SIPROTEC clears TrgOps to 0x0 whenever step 1
     * writes a DIFFERENT DatSet. An RCB that arrived already carrying
     * dchg|qchg|gi therefore had its triggers wiped by step 1, while this
     * skip-condition - reading the pre-step-1 snapshot - still saw 0x13 and
     * skipped the rewrite. The result was an RCB enabled with TrgOps=0x0: it
     * can never emit a change report, and because TrgOps.gi is gone it doesn't
     * even answer step 6's GI, so it produces literally nothing while every
     * write in the sequence returns IED_ERROR_OK. Three of eighteen enabled
     * RCBs on a real device were silently dark this way, with the cycle
     * summary reporting "no failures" - see CHANGELOG.md.
     *
     * A FAILED read-back falls through to writing, never to skipping - same
     * reasoning as step 2. */
    int neededTrgOps = TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI;
    bool trgOpsAlreadySet = postDatSetState.readOk
            && (postDatSetState.trgOps & neededTrgOps) == neededTrgOps;
    if (!trgOpsAlreadySet) {
        int deviceTrgOps = postDatSetState.readOk ? postDatSetState.trgOps : 0;
        int desiredTrgOps = deviceTrgOps | neededTrgOps;
        ClientReportControlBlock_setTrgOps(rcb, desiredTrgOps);
        char intended[256];
        snprintf(intended, sizeof(intended), "TrgOps=0x%x (device had 0x%x after the DatSet write%s, OR'ing "
                "in dchg|qchg|gi 0x%x)", desiredTrgOps, deviceTrgOps,
                postDatSetState.readOk ? "" : " - read-back failed, assuming none set", neededTrgOps);
        RcbStep trgOpsStep = {
            .element = RCB_ELEMENT_TRG_OPS,
            .name = "3/6 TrgOps",
            .intendedText = intended,
            .verify = RCB_STEP_VERIFY_TRG_OPS,
            .expectedBits = neededTrgOps,
        };
        bool trgOpsApplied = false;
        IedClientError trgOpsErr = runRcbStep(handle, rcb, target->objectReference, &trgOpsStep, &trgOpsApplied, NULL);
        if (trgOpsErr != IED_ERROR_OK || !trgOpsApplied) {
            /* Not fatal here - the post-sequence check below is what decides
             * whether this RCB is genuinely dead, since a partial TrgOps
             * (dchg/qchg present, gi refused) still reports changes fine. */
            fprintf(stderr, "[mms_report_client] '%s' step 3 did not take effect - this RCB will be enabled "
                    "with whatever TrgOps the device already has; the post-sequence check below decides "
                    "whether that leaves it able to report at all\n", target->objectReference);
        }
    } else {
        /* Prints the post-DatSet value actually judged. */
        fprintf(stderr, "[mms_report_client] '%s' step '3/6 TrgOps': skipped (device still has dchg|qchg|gi "
                "set after the DatSet write, TrgOps=0x%x)\n", target->objectReference, postDatSetState.trgOps);
    }

    /* Step 4. Resume a buffered RCB's delivery from the last EntryID this
     * client actually received, instead of re-requesting the server's entire
     * unacknowledged backlog on every RptEna transition - see
     * MmsReportClientMemberRefCacheEntry.lastEntryId's own doc comment for
     * why this matters (a redelivered multi-entry backlog defeats the
     * single-slot value-diff cache). RCB_ELEMENT_ENTRY_ID is only meaningful
     * for buffered RCBs (iec61850_client.h) - gated on target->buffered.
     * lastEntryId is written by the report-adapter thread, so this read goes
     * through the same memberRefCacheLock that guards it;
     * ClientReportControlBlock_setEntryId itself is a local, synchronous
     * struct mutation (confirmed against libiec61850's own source - it clones
     * the value internally), not a network call, so it's safe to make while
     * holding the lock, unlike the IedConnection_setRCBValues inside
     * runRcbStep, which is deliberately outside it. On the very first-ever
     * enable (lastEntryId still NULL - nothing to resume from yet) this step
     * is skipped entirely, same full-backlog behavior as before this feature
     * existed. Computed BEFORE the GI decision just below - hasResumableEntryId
     * is that decision's own input (MmsReportClientUseCases_shouldRequestGiOnEnable). */
    bool hasResumableEntryId = false;
    if (target->buffered) {
        MmsReportClientMemberRefCacheEntry* cacheEntry = lookupMemberRefCacheByRcb(handle, target->objectReference);
        if (cacheEntry) {
            Semaphore_wait(handle->memberRefCacheLock);
            if (cacheEntry->lastEntryId) {
                ClientReportControlBlock_setEntryId(rcb, cacheEntry->lastEntryId);
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
     * `reason` is still never trusted for filtering (see that same doc
     * comment). Not const - step 4's own rejection handling below can still
     * force it true. BUF_TM/INTG_PD/CONF_REV, and TrgOps.dupd/integrity
     * specifically, are never touched anywhere in this sequence, staying
     * exactly as the IED's own config has them. */
    bool requestGi = MmsReportClientUseCases_shouldRequestGiOnEnable(target->buffered, hasResumableEntryId);

    if (hasResumableEntryId) {
        RcbStep entryIdStep = {
            .element = RCB_ELEMENT_ENTRY_ID,
            .name = "4/6 EntryID",
            .intendedText = "the cached EntryID as this RCB's resume point",
            .verify = RCB_STEP_VERIFY_NONE,
        };
        IedClientError entryIdErr = runRcbStep(handle, rcb, target->objectReference, &entryIdStep, NULL, NULL);
        if (entryIdErr != IED_ERROR_OK) {
            /* Unlike the old bundled write - where an EntryID rejection had
             * to be INFERRED from a combined failure that could equally have
             * been about DatSet or TrgOps - this write carried nothing but
             * the EntryID, so its rejection is unambiguous. The server no
             * longer recognizes the EntryID we cached: its own buffer wrapped
             * past it after a very long disconnect, or the server itself
             * restarted. IEC 61850 leaves the exact failure mode here
             * implementation-defined, so rather than guess at it, fall back
             * to the pre-existing full-resume behavior instead of leaving
             * this RCB unreported - non-fatal, the sequence continues to
             * step 5.
             *
             * Clear the cached EntryID back to NULL rather than leaving it in
             * place. Required alongside MmsReportClientReportAdapter_onReport's
             * own EntryID-staleness guard (mms_report_client_report_adapter.c):
             * that guard drops any incoming report whose EntryID isn't strictly
             * greater than this cached value, and only ever advances it on a
             * report that survives the guard - so if a restarted server's own
             * counter legitimately restarts low, a stale cached value here
             * would make every one of its fresh reports look "stale" too,
             * permanently silencing this RCB until the whole daemon process
             * restarts. This reset is the one signal we actually have that the
             * old baseline can no longer be trusted, so it puts the guard back
             * into its fail-open, "nothing to compare against yet" bootstrap
             * state.
             *
             * This same rejection is also the definitive signal the device's
             * report state was reset (most commonly a real device reboot) - a
             * subsequently recreated dataset holds genuinely fresh values even
             * when it has the exact same deterministic name as before (the
             * everyday reconnect case ensureLnFallbackMemberRefCache's own
             * name-based check is built for), so the value-diff cache must be
             * reset to bootstrap here too, not just lastEntryId - otherwise the
             * next report diffs fresh post-reset values against stale
             * pre-reset ones and nearly everything reads as "changed."
             * MmsReportClientUseCases_resetValueDiffCacheToBootstrap's own doc
             * comment has the full reasoning.
             *
             * GI was skipped above precisely because we had a resumable
             * EntryID - now that the server has rejected it and the cache is
             * about to be cleared, step 6 needs the same GI safety net a
             * genuine first-ever enable gets. */
            fprintf(stderr, "[mms_report_client] '%s' step 4 failed but is NON-FATAL - the device rejected "
                    "the cached EntryID, falling back to a full resume (gi=true) for this enable\n",
                    target->objectReference);
            requestGi = true;

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
    } else {
        fprintf(stderr, "[mms_report_client] '%s' step '4/6 EntryID': skipped (%s)\n", target->objectReference,
                target->buffered ? "no cached EntryID to resume from yet - this enable is a full resume"
                                 : "unbuffered RCB - EntryID is a buffered-only concept");
    }

    /* Step 5 - and only now, with the dataset bound and every configuration
     * attribute already written while the RCB was disabled, is it actually
     * enabled. */
    ClientReportControlBlock_setRptEna(rcb, true);
    /* Steps 5 and 6 each hand back their own read-back; whichever ran last is
     * the device's final word on this RCB and feeds the can-it-actually-report
     * check below. */
    RcbLiveState finalState;
    RcbLiveState giState;
    memset(&finalState, 0, sizeof(finalState));
    memset(&giState, 0, sizeof(giState));
    bool rptEnaApplied = false;
    {
        RcbStep rptEnaStep = {
            .element = RCB_ELEMENT_RPT_ENA,
            .name = "5/6 RptEna=true",
            .intendedText = "RptEna=true",
            .verify = RCB_STEP_VERIFY_RPT_ENA,
            .expectedBool = true,
        };
        err = runRcbStep(handle, rcb, target->objectReference, &rptEnaStep, &rptEnaApplied, &finalState);
    }
    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] '%s' ABORTING enable at step 5 - the device refused to enable "
                "reporting on this RCB\n", target->objectReference);
        IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false, err);
        }
        free(effectiveDatasetReference);
        destroyRcbLiveState(&finalState);
        destroyRcbLiveState(&giState);
        destroyRcbLiveState(&postDatSetState);
        destroyRcbLiveState(&entryState);
        ClientReportControlBlock_destroy(rcb);
        return RCB_ENABLE_OUTCOME_FAILED;
    }
    /* rptEnaApplied is deliberately NOT judged here - a device is not obliged
     * to reflect the change on an immediate read-back, and this client has no
     * basis to overrule an explicit IED_ERROR_OK. It is reported alongside the
     * can-it-actually-report verdict at the end of this function, where both
     * flavours of "enabled on paper only" are surfaced together. */

    /* Step 6. */
    if (requestGi) {
        ClientReportControlBlock_setGI(rcb, true);
        RcbStep giStep = {
            .element = RCB_ELEMENT_GI,
            .name = "6/6 GI",
            .intendedText = "GI=true (one-shot general interrogation against the now-active RCB)",
            .verify = RCB_STEP_VERIFY_NONE,
        };
        IedClientError giErr = runRcbStep(handle, rcb, target->objectReference, &giStep, NULL, &giState);
        if (giErr != IED_ERROR_OK) {
            fprintf(stderr, "[mms_report_client] '%s' step 6 failed but is NON-FATAL - the RCB is bound and "
                    "enabled, it just won't produce the initial snapshot this enable asked for\n",
                    target->objectReference);
        }
    } else {
        fprintf(stderr, "[mms_report_client] '%s' step '6/6 GI': skipped (buffered RCB resuming from a valid "
                "EntryID - the backlog already covers everything missed, see this function's own GI doc "
                "comment)\n", target->objectReference);
    }

    /* ---- Can this RCB actually report? ----------------------------------
     *
     * No extra read-back needed: whichever step ran last (6 if GI ran, else 5)
     * already fetched the device's own live state, and that is the device's
     * final word on this RCB.
     *
     * Every write in the sequence can return IED_ERROR_OK and still leave an
     * RCB that emits nothing whatsoever - that is exactly what happened on a
     * real SIPROTEC when step 3 was judged from a stale snapshot (see step 3's
     * own doc comment). An enable that produces a permanently silent RCB is a
     * failure, not a success, and reporting it as "enabled" made the cycle
     * summary's "no failures" actively misleading while ~102 attributes went
     * unmonitored.
     *
     * Judged on the CHANGE triggers specifically, not on the full dchg|qchg|gi
     * set: without dchg and without qchg the RCB can never produce an
     * event-driven report at all, which is fatal. Missing only TRG_OPT_GI is
     * degraded but alive - change reports still flow, this enable just loses
     * its one-time snapshot - so that stays a warning rather than a failure,
     * and a device that legitimately refuses only the GI bit is not written
     * off. An unreadable final state is not treated as fatal either; there is
     * no evidence of death, only absence of evidence. */
    const RcbLiveState* lastState = giState.readOk ? &giState : &finalState;
    int changeTriggers = TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED;
    bool cannotReport = lastState->readOk && (lastState->trgOps & changeTriggers) == 0;

    if (cannotReport) {
        fprintf(stderr, "[mms_report_client] '%s' FAILED - every write returned OK, but the device's own live "
                "TrgOps is 0x%x: neither dchg nor qchg is set, so this RCB can never emit a change report "
                "(and with TrgOps.gi absent it won't answer step 6's GI either). It is enabled on paper and "
                "silent in practice - counted as a failure, not an enable\n",
                target->objectReference, lastState->trgOps);
        IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false,
                    IED_ERROR_OBJECT_ATTRIBUTE_INCONSISTENT);
        }
        free(effectiveDatasetReference);
        destroyRcbLiveState(&finalState);
        destroyRcbLiveState(&giState);
        destroyRcbLiveState(&postDatSetState);
        destroyRcbLiveState(&entryState);
        ClientReportControlBlock_destroy(rcb);
        return RCB_ENABLE_OUTCOME_FAILED;
    }

    if (lastState->readOk && !(lastState->trgOps & TRG_OPT_GI)) {
        fprintf(stderr, "[mms_report_client] '%s' WARNING: enabled and able to report changes (TrgOps=0x%x), "
                "but TrgOps.gi is not set - this device will ignore step 6's GI, so there is no initial "
                "snapshot and the value-diff cache seeds from the first real change instead\n",
                target->objectReference, lastState->trgOps);
    }
    if (!rptEnaApplied) {
        fprintf(stderr, "[mms_report_client] '%s' WARNING: the device accepted RptEna=true but its own "
                "read-back did not show this RCB as enabled - reporting may never start on it. Treated as "
                "success anyway (a device may simply not reflect the change immediately); this is the line "
                "to look at first if no reports arrive from this RCB\n", target->objectReference);
    }

    fprintf(stderr, "[mms_report_client] enabled reporting for '%s' (buffered=%d, dataset='%s', gi=%d)\n",
            target->objectReference, target->buffered, effectiveDatasetReference, requestGi);

    if (handle->rcbStatusCallback) {
        handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, true, IED_ERROR_OK);
    }

    free(effectiveDatasetReference);
    destroyRcbLiveState(&finalState);
    destroyRcbLiveState(&giState);
    destroyRcbLiveState(&postDatSetState);
    destroyRcbLiveState(&entryState);
    ClientReportControlBlock_destroy(rcb);
    return RCB_ENABLE_OUTCOME_ENABLED;
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
 *     MmsReportClientUseCases_chunkReferencesAcrossWholeDevice - a single
 *     resulting cluster may legitimately span several different (small) LNs'
 *     worth of leaves when they fit together, maximizing how much of the
 *     device fits within a tight total dataset-count budget.
 *   - UNKNOWN (-1 or 0 on both caps, e.g. no <Services> at all, or a
 *     dynamically-built online-discovered model): MmsReportClientUseCases_groupReferencesByLn
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
        fprintf(stderr, "[mms_report_client] whole-device cluster plan: this device has NO Dyn RCB slots (every "
                "RCB already carries an SCL-declared datSet) - nothing to plan\n");
        LinkedList_destroyStatic(slots);
        return plan;
    }
    fprintf(stderr, "[mms_report_client] whole-device cluster plan: %d Dyn RCB slot(s) available\n",
            LinkedList_size(slots));
    for (LinkedList slotLog = LinkedList_getNext(slots); slotLog; slotLog = LinkedList_getNext(slotLog)) {
        ReportControlBlockTarget* t = (ReportControlBlockTarget*) LinkedList_getData(slotLog);
        fprintf(stderr, "[mms_report_client]   Dyn slot: '%s' (LN '%s', buffered=%d)\n",
                t->objectReference, t->lnReference ? t->lnReference : "(none)", t->buffered);
    }

    LinkedList wholeDeviceLeaves = IedModel_getReportableAttributeReferencesForWholeDevice(handle->iedModel);
    int leafCount = wholeDeviceLeaves ? LinkedList_size(wholeDeviceLeaves) : 0;
    if (leafCount == 0) {
        fprintf(stderr, "[mms_report_client] whole-device cluster plan: the model has NO reportable (FC=ST/MX) "
                "attributes anywhere - no Dyn RCB can be given a dataset this cycle\n");
        if (wholeDeviceLeaves) LinkedList_destroyDeep(wholeDeviceLeaves, free);
        LinkedList_destroyStatic(slots);
        return plan;
    }
    fprintf(stderr, "[mms_report_client] whole-device cluster plan: %d reportable attribute(s) across the "
            "whole device to distribute\n", leafCount);

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
     * (below) if neither is - same UNKNOWN posture as before this change. */
    int confMaxAttributes = IedModel_getConfDataSetMaxAttributes(handle->iedModel);
    int dynMaxAttributes = IedModel_getDynDataSetMaxAttributes(handle->iedModel);
    int maxAttributes = confMaxAttributes > 0 ? confMaxAttributes : dynMaxAttributes;
    fprintf(stderr, "[mms_report_client] whole-device cluster plan: SCL declares ConfDataSet maxAttributes=%d, "
            "DynDataSet maxAttributes=%d - using cap %d from %s, strategy=%s\n",
            confMaxAttributes, dynMaxAttributes, maxAttributes,
            confMaxAttributes > 0 ? "ConfDataSet" : (dynMaxAttributes > 0 ? "DynDataSet" : "neither (undeclared)"),
            maxAttributes > 0 ? "DO-atomic bin-packing across the whole device"
                              : "one dataset per LN (no known size bound to pack against)");
    LinkedList clusterLists = (maxAttributes > 0)
            ? MmsReportClientUseCases_chunkReferencesAcrossWholeDevice(
                    (const char* const*) leafArray, leafCount, maxAttributes)
            : MmsReportClientUseCases_groupReferencesByLn((const char* const*) leafArray, leafCount);
    free(leafArray);
    LinkedList_destroyDeep(wholeDeviceLeaves, free);

    int totalClusters = LinkedList_size(clusterLists);
    int slotCount = LinkedList_size(slots);
    int assignedClusters = 0;
    fprintf(stderr, "[mms_report_client] whole-device cluster plan: chunking produced %d cluster(s) for %d "
            "slot(s)\n", totalClusters, slotCount);

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
                /* The plan itself, one line per assignment. Which slot got
                 * which part of the device is otherwise only inferable by
                 * correlating createDataSet lines after the fact, and a
                 * cluster whose size exceeds the device's real (as opposed to
                 * SCL-declared) cap is only diagnosable if the intended size
                 * is on record before the create is attempted. */
                fprintf(stderr, "[mms_report_client]   cluster %d/%d -> slot '%s' (%d member(s))\n",
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
         * disabling RptEna alone is not enough to unblock deletion
         * (confirmed against the reference server -
         * IED_ERROR_OBJECT_CONSTRAINT_CONFLICT/35, see
         * test_deleteDataSet_refusedWhileRcbEnabled_succeedsAfterDisable) -
         * the RCB's own DatSet attribute continuing to point at this orphan
         * is itself the constraint. A crashed daemon never reached
         * MmsReportClientConnection_stop's own disable+unbind at all, so
         * matchedTarget's RptEna/DatSet may genuinely still be live
         * server-side here exactly as if this were the graceful path - this
         * proactive pass needs the identical two-step sequence before its
         * own delete attempt below. Scoped the same way too: only touches
         * matchedTarget's RCB if its CURRENT live DatSet actually is this
         * candidate, never blind. */
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
                    fprintf(stderr, "[mms_report_client] could not disable/unbind '%s' (dataset '%s') "
                            "before orphan cleanup: %s (%d)\n", matchedTarget->objectReference, candidate,
                            IedClientError_toString(disableErr), disableErr);
                } else {
                    fprintf(stderr, "[mms_report_client] disabled/unbound '%s' from orphaned dataset '%s' "
                            "before cleanup\n", matchedTarget->objectReference, candidate);
                }
            } else {
                fprintf(stderr, "[mms_report_client] orphan candidate '%s' matches '%s's own deterministic "
                        "name but its live DatSet is '%s' - not currently bound to it, skipping disable/"
                        "unbind\n", candidate, matchedTarget->objectReference, liveDataSet ? liveDataSet : "(none)");
            }
            ClientReportControlBlock_destroy(orphanRcb);
        }

        IedClientError deleteErr = IED_ERROR_OK;
        if (IedConnection_deleteDataSet(handle->connection, &deleteErr, candidate)) {
            fprintf(stderr, "[mms_report_client] cleaned up orphaned dataset '%s' - not needed by any "
                    "target this cycle, reclaiming budget\n", candidate);
        } else {
            fprintf(stderr, "[mms_report_client] could not clean up orphaned dataset '%s': %s (%d) - "
                    "left behind\n", candidate, IedClientError_toString(deleteErr), deleteErr);
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
     * that same doc comment for why only Conf gets a discovery correction. */
    int sclDynMax = IedModel_getDynDataSetMax(handle->iedModel);
    int sclConfMax = IedModel_getConfDataSetMax(handle->iedModel);
    int existingCount = LinkedList_size(existingServerDatasets);
    DynamicDatasetSession session = {
        .cache = LinkedList_create(),
        .remainingDynBudget = MmsReportClientUseCases_computeInitialDynamicDatasetBudget(sclDynMax, 0),
        .dynBudgetExhaustedLogged = false,
        .remainingConfBudget = MmsReportClientUseCases_computeInitialDynamicDatasetBudget(sclConfMax, existingCount),
        .confBudgetExhaustedLogged = false,
        .chunkAssignments = buildWholeDeviceClusterPlan(handle),
        .existingServerDatasets = existingServerDatasets,
        .claimedDatasetNames = LinkedList_create(),
        .associationSpecificCreateRejected = false,
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

    int enabledCount = 0;
    int notNeededCount = 0;
    int failedCount = 0;
    LinkedList element = LinkedList_getNext(handle->targets);
    while (element && !handle->stopRequested) {
        switch (enableOneTarget(handle, (ReportControlBlockTarget*) LinkedList_getData(element), &session)) {
            case RCB_ENABLE_OUTCOME_ENABLED:    enabledCount++; break;
            case RCB_ENABLE_OUTCOME_NOT_NEEDED: notNeededCount++; break;
            case RCB_ENABLE_OUTCOME_FAILED:     failedCount++; break;
            case RCB_ENABLE_OUTCOME_SKIPPED_STOPPING: break; /* teardown, not an outcome worth counting */
        }
        element = LinkedList_getNext(element);
    }

    /* The one number a real-hardware log capture is actually read for. Without
     * it, answering "did anything really fail this cycle?" means counting
     * per-RCB lines by hand across a capture that now runs to dozens of lines
     * per RCB. Suppressed entirely mid-teardown, where a partial tally would
     * be actively misleading. The failure clause is only named when there
     * genuinely are failures, so a healthy cycle reads clean rather than
     * ending on the word FAILED every time. */
    if (!handle->stopRequested) {
        int total = enabledCount + notNeededCount + failedCount;
        if (failedCount > 0) {
            fprintf(stderr, "[mms_report_client] enable cycle complete for %d RCB(s): %d enabled, %d not needed "
                    "(device already fully covered), %d FAILED - see the per-RCB lines above\n",
                    total, enabledCount, notNeededCount, failedCount);
        } else {
            fprintf(stderr, "[mms_report_client] enable cycle complete for %d RCB(s): %d enabled, %d not needed "
                    "(device already fully covered), no failures\n", total, enabledCount, notNeededCount);
        }
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
                                "before dataset cleanup: %s (%d)\n", target->objectReference, liveDataSet,
                                IedClientError_toString(err), err);
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
                        "%s (%d) - left behind on the device\n", datasetName,
                        IedClientError_toString(deleteErr), deleteErr);
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
