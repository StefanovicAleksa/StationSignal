#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_report_client/data/mms_report_client_connection.h"
#include "features/mms_report_client/data/mms_report_client_report_adapter.h"
#include "features/mms_report_client/data/mms_report_client_auth.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"
#include "features/mms_dataset_manager/service/mms_dataset_manager_api.h"
#include "hal_time.h"
#include "mms_value.h"

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

/*
 * Rebuilds (or confirms up to date) this RCB's memberRefCache entry to match
 * `liveDataset`'s real shape - called from enableOneTarget on every
 * (re)connect where tier 2/3 (pull live / adopt) succeeded. Compares liveDataset
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

/*
 * Tier 4's counterpart of refreshPulledMemberRefCache, called once
 * effectiveDatasetReference is known to be mms_dataset_manager's own
 * deterministic per-target self-created name (or NULL, if even self-creation
 * failed - a no-op, nothing to reconcile). Because that name/shape is 100%
 * deterministic per target and was ALREADY built into this entry as the
 * cluster-fallback PROVISIONAL shape at MmsReportClient_start time
 * (buildMemberRefCache, mms_report_client_api.c - tagged with a NULL
 * resolvedDatasetReference precisely so this first call always confirms it),
 * this is bookkeeping-only in the overwhelmingly common case: first-ever
 * successful tier-4 enable, or any reconnect after that (the name is
 * deterministic, so it matches every time) just stamps the fingerprint under
 * the lock - zero array rebuilding, zero extra calls beyond createDataSet
 * itself. The only case this actually rebuilds the shape is a transition BACK
 * from a previously-active tier-2 pulled dataset (resolvedDatasetReference
 * held some prior live dataset's name, now gone/unresolvable this cycle) - in
 * that one case, the shape is recomputed from `memberRefs` and swapped in
 * exactly like refreshPulledMemberRefCache's own shape-swap, with the same
 * value-diff-cache-reset consequence and the same rationale.
 *
 * `memberRefs` is MmsDatasetResolution.memberReferences for a
 * MMS_DATASET_TIER_SELF_CREATED resolution: this target's own whole-device
 * cluster members, as planned by mms_dataset_manager. NULL and EMPTY mean
 * different things here, exactly as that field documents - NULL is "no cluster
 * was assigned at all", nothing to reconcile and no fingerprint to stamp;
 * empty is "a cluster exists but holds no members", which still stamps the
 * fingerprint so the rebuild isn't retried on every single enable.
 *
 * REQUIRED for correctness, not just an optimization: the dataset manager only
 * ever returns a self-created name when this target has a real cluster
 * assignment (whole-device clustering has no "unchunked, whole-LN" fallback -
 * a target with no assignment gets no dataset at all) - the rebuilt shape here
 * must come from that exact same cluster's own member subset, or a
 * correctly-sized, cluster-scoped dataset would exist on the wire while this
 * entry's decode-time shape silently diverges, corrupting report decoding on
 * this target's very first enable.
 */
static void
ensureLnFallbackMemberRefCache(MmsReportClientHandle handle, ReportControlBlockTarget* target,
        const char* effectiveDatasetReference, LinkedList memberRefs) {
    if (!effectiveDatasetReference || !memberRefs) return;

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

    int count = LinkedList_size(memberRefs);
    char** array = NULL;
    if (count > 0) {
        array = calloc((size_t) count, sizeof(char*));
        if (!array) {
            free(previousDatasetReference);
            return;
        }
        int i = 0;
        for (LinkedList el = LinkedList_getNext(memberRefs); el; el = LinkedList_getNext(el)) {
            array[i++] = MmsReportClientUtils_safeStringDup((char*) LinkedList_getData(el));
        }
    }

    if (count <= 0) {
        /* Nothing reportable - still stamp the fingerprint so this rebuild
         * isn't retried on every single enable. No actual cache reset happens
         * here (no fresh entry is built/swapped below), so no reset log line -
         * see the real swap path below. */
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
enableOneTarget(MmsReportClientHandle handle, ReportControlBlockTarget* target) {
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
     * WHICH dataset to bind is entirely mms_dataset_manager's decision - see
     * MmsDatasetManager_resolveForTarget (mms_dataset_manager_api.h) for the
     * full four-tier order (SCL static -> pull live -> adopt unclaimed ->
     * self-create) and every rule behind it. This feature only consumes the
     * answer: bind it, enable the RCB, decode its reports.
     *
     * Tiers 2/3 no longer read `rcb` here at all - mms_dataset_manager's own
     * claim pass (MmsDatasetManagerProvisioning_runClaimPass, run once for
     * every target from MmsDatasetManager_beginCycle, before this cycle's
     * whole-device cluster plan even exists) already resolved them with its
     * own separately-fetched RCB, so this call is a pure lookup for tiers 1-3
     * and only touches the wire itself for a genuine tier-4 self-create.
     *
     * The resolution owns its own strings; resolution.datasetReference is an
     * OWNED copy, deliberately not a borrowed alias into anything internal to
     * mms_dataset_manager - see MmsDatasetResolution's own doc comment
     * (mms_dataset_manager_types.h) for the aliasing hazard this avoids.
     *
     * Every exit path from here on MUST MmsDatasetManager_destroyResolution. */
    MmsDatasetResolution resolution;
    MmsDatasetManager_resolveForTarget(handle->datasetManager, target, &resolution);

    /* Decode-shape reconciliation stays HERE, not in the dataset manager: it
     * mutates THIS feature's own memberRefCache under THIS feature's own
     * memberRefCacheLock, against the report-adapter thread. The tier only
     * decides which of the two reconciliation paths applies.
     *
     * Tier 1 (SCL) needs neither - buildMemberRefCache already resolved that
     * shape locally at start time from the same immutable datasetReference,
     * and it can never change for this target's lifetime. */
    if (resolution.tier == MMS_DATASET_TIER_LIVE || resolution.tier == MMS_DATASET_TIER_ADOPTED) {
        /* A live/adopted dataset's real member list came off the wire and may
         * match nothing known locally - reconcile against exactly what the
         * device reported. */
        refreshPulledMemberRefCache(handle, target, resolution.datasetReference, resolution.memberReferences);
    } else if (resolution.tier == MMS_DATASET_TIER_SELF_CREATED) {
        /* A self-created dataset's members are precisely the cluster the
         * dataset manager planned for this target - reconcile against that.
         * No-op when nothing was created (NULL reference/member list). */
        ensureLnFallbackMemberRefCache(handle, target, resolution.datasetReference, resolution.memberReferences);
    }

    /* Which tier actually resolved the dataset - or that none did, the same
     * "nothing to bind, nothing to enable" case handled just below. Logged
     * unconditionally (not just on failure) so a real run shows, per RCB,
     * exactly which path was taken without having to infer it from whichever
     * failure fprintf's did or didn't fire. Deliberately emitted HERE rather
     * than inside the dataset manager, so the reconciliation above still logs
     * its own "dataset identity changed" line BEFORE this one, exactly as it
     * did when both lived in this function. */
    fprintf(stderr, "[mms_report_client] '%s' dataset resolved via %s: '%s'\n", target->objectReference,
            resolution.datasetReference ? resolution.tierName : "none",
            resolution.datasetReference ? resolution.datasetReference : "(none)");
    if (!resolution.datasetReference) {
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
         * under a pile of benign ones. MmsDatasetResolution.wasNeeded (see
         * its own doc comment, mms_dataset_manager_types.h) separates them. */
        if (!resolution.wasNeeded) {
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
            MmsDatasetManager_destroyResolution(&resolution);
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
        MmsDatasetManager_destroyResolution(&resolution);
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
    bool datSetAlreadyBound = false;
    ClientReportControlBlock_setDataSetReference(rcb, resolution.datasetReference);
    {
        char intended[512];
        snprintf(intended, sizeof(intended), "DatSet='%s' (resolved via %s)", resolution.datasetReference,
                resolution.tierName);
        RcbStep datSetStep = {
            .element = RCB_ELEMENT_DATSET,
            .name = "1/6 DatSet",
            .intendedText = intended,
            .verify = RCB_STEP_VERIFY_DATSET,
            .expectedString = resolution.datasetReference,
        };
        err = runRcbStep(handle, rcb, target->objectReference, &datSetStep, &datSetAlreadyBound, &postDatSetState);
    }
    /*
     * A REFUSED write is only fatal if the binding we wanted isn't already in
     * place. `datSetAlreadyBound` is runRcbStep's own RCB_STEP_VERIFY_DATSET
     * read-back result, computed from the device's live state regardless of
     * whether the write itself succeeded - so this costs no extra round trip.
     *
     * The case this exists for: SCL's <Services><ReportSettings datSet="Conf">
     * declares DatSet configurable OFFLINE ONLY, not online-writable, and a
     * device that enforces it refuses every DatSet write. Confirmed declared
     * that way on every ABB IED in a real 48-IED station file, against
     * Siemens' datSet="Dyn" in both of its own, which is why this never
     * surfaced before. What we write is that RCB's own SCL-declared datSet -
     * i.e. the binding the device already has - so a refusal there says
     * nothing about whether the RCB can report, and aborting would take down
     * every RCB on the device (231 of them on that file).
     *
     * Still fatal when the read-back does NOT confirm the dataset (including
     * a read-back that itself failed, which leaves this false): the RCB would
     * then be reporting on something other than what was resolved, or on
     * nothing at all, and every later step's judgment about it would be
     * meaningless.
     */
    if (err != IED_ERROR_OK && datSetAlreadyBound) {
        fprintf(stderr, "[mms_report_client] '%s' step '1/6 DatSet': write was refused, but the device already "
                "reports exactly the dataset we resolved ('%s') - the binding is correct, continuing the enable "
                "(a device declaring ReportSettings datSet=\"Conf\"/\"Fix\" refuses the write by design)\n",
                target->objectReference, resolution.datasetReference);
        err = IED_ERROR_OK;
    }
    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] '%s' ABORTING enable at step 1 - the dataset binding itself was "
                "rejected and the device does not already report the dataset we resolved, so there is nothing "
                "left to enable\n", target->objectReference);
        IedConnection_uninstallReportHandler(handle->connection, target->objectReference);
        if (handle->rcbStatusCallback) {
            handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, false, err);
        }
        MmsDatasetManager_destroyResolution(&resolution);
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
        MmsDatasetManager_destroyResolution(&resolution);
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
        MmsDatasetManager_destroyResolution(&resolution);
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
            target->objectReference, target->buffered, resolution.datasetReference, requestGi);

    if (handle->rcbStatusCallback) {
        handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, true, IED_ERROR_OK);
    }

    MmsDatasetManager_destroyResolution(&resolution);
    destroyRcbLiveState(&finalState);
    destroyRcbLiveState(&giState);
    destroyRcbLiveState(&postDatSetState);
    destroyRcbLiveState(&entryState);
    ClientReportControlBlock_destroy(rcb);
    return RCB_ENABLE_OUTCOME_ENABLED;
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

    /* Opens this connect cycle's dataset session: server-side discovery of
     * what already exists, both budgets seeded (and the ConfDataSet one
     * corrected against that discovery), and the whole-device cluster plan
     * assigning each spare Dyn RCB slot its own share of the device's data.
     * Nothing here survives a reconnect - the device's own state may have
     * genuinely changed since the last one, so it all re-runs fresh. */
    MmsDatasetManager_beginCycle(handle->datasetManager);

    int enabledCount = 0;
    int notNeededCount = 0;
    int failedCount = 0;
    LinkedList element = LinkedList_getNext(handle->targets);
    while (element && !handle->stopRequested) {
        switch (enableOneTarget(handle, (ReportControlBlockTarget*) LinkedList_getData(element))) {
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

    /* Closes the cycle: runs the proactive orphan-cleanup pass (reclaiming
     * budget from our own domain-scoped datasets no target needed this cycle -
     * the ungraceful-restart gap the stop path can't close), then tears the
     * session down. Cleanup is skipped mid-teardown, where the connection is
     * about to go away and a best-effort delete pass is just noise. */
    MmsDatasetManager_endCycle(handle->datasetManager, !handle->stopRequested);
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

    /* Releases every dataset this client itself created on the device:
     * disables RptEna and clears DatSet on any RCB still bound to one, then
     * deletes them. MUST run here - before IedConnection_close below - because
     * both steps need a live association. Scoped strictly to our own
     * datasets; an SCL-static target's engineering configuration and any
     * foreign/adopted dataset are never touched. See
     * MmsDatasetManager_cleanupOnStop for the full contract. */
    MmsDatasetManager_cleanupOnStop(handle->datasetManager);

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
