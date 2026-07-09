#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/mms_report_client/data/mms_report_client_connection.h"
#include "features/mms_report_client/data/mms_report_client_report_adapter.h"
#include "features/mms_report_client/data/mms_report_client_auth.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"

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

    IedClientError err = IED_ERROR_OK;
    IedConnection_createDataSet(handle->connection, &err, datasetName, wireRefs);
    LinkedList_destroyDeep(wireRefs, free);

    if (err != IED_ERROR_OK) {
        fprintf(stderr, "[mms_report_client] dynamic dataset creation failed for LN '%s': error %d - "
                "'%s' will not report\n", target->lnReference, err, target->objectReference);
        free(datasetName);
        return NULL;
    }

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

    uint32_t mask = RCB_ELEMENT_RPT_ENA;

    /* DatSet must be (re-)set explicitly on enable - relying on a
     * server-side default dataset (configured only via ReportControlBlock_create's
     * dataSetName at server build time) is fragile: libiec61850's own
     * reference client example (client_example_no_thread.c) always sets
     * RCB_ELEMENT_DATSET alongside RPT_ENA/GI too, using the same "$"-joined
     * reference format ied_model already hands us in datasetReference.
     *
     * If neither SCL nor a prior client has assigned this RCB a dataset, this
     * is a "dynamic" RCB (IEC 61850 permits an RCB to exist with no dataset
     * until one is assigned at runtime, datSet="Dyn" in SCL terms) -
     * getOrCreateDynamicDataset synthesizes one covering every reportable
     * (FC=ST/MX) attribute of the RCB's own LN, association-scoped so it
     * needs no explicit cleanup. If that also fails (no reportable
     * attributes, or the device rejects creation - cap exceeded, etc.),
     * DATSET is left unset and setRCBValues below fails with
     * IED_ERROR_OBJECT_VALUE_INVALID, same as before this feature existed. */
    const char* effectiveDatasetReference = target->datasetReference;
    if (!effectiveDatasetReference) {
        effectiveDatasetReference = getOrCreateDynamicDataset(handle, target, dynamicDatasetCache);
    }
    if (effectiveDatasetReference) {
        ClientReportControlBlock_setDataSetReference(rcb, effectiveDatasetReference);
        mask |= RCB_ELEMENT_DATSET;
    }

    ClientReportControlBlock_setRptEna(rcb, true);
    if (handle->config.generalInterrogationOnEnable) {
        ClientReportControlBlock_setGI(rcb, true);
        mask |= RCB_ELEMENT_GI;
    }
    /* Deliberately never touches TRG_OPS/BUF_TM/INTG_PD/CONF_REV - those stay
     * exactly as the IED's own SCL config already has them. */

    IedConnection_setRCBValues(handle->connection, &err, rcb, mask, true);
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

    if (handle->rcbStatusCallback) {
        handle->rcbStatusCallback(handle->rcbStatusCallbackParam, target->objectReference, true, IED_ERROR_OK);
    }

    /* Every successful (re-)enable forgets whatever was cached before -
     * first connect included (harmless there, the cache is already all-NULL)
     * - so the GI snapshot this enable just requested is never diffed
     * against stale pre-disconnect values and silently dropped. See
     * MmsReportClientUseCases_resetValueDiffCache's own doc comment. */
    MmsReportClientMemberRefCacheEntry* cacheEntry = lookupMemberRefCacheByRcb(handle, target->objectReference);
    if (cacheEntry) MmsReportClientUseCases_resetValueDiffCache(cacheEntry);

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
            handle->currentBackoffMs = 0;
            enableAllTargets(handle);

            /* Block until onStateChanged signals an unexpected loss, or
             * stop() posts to unblock us. */
            Semaphore_wait(handle->wakeSignal);

            if (handle->stopRequested) break;
            if (!handle->connectionLostSignal) continue; /* spurious wake guard */
            handle->connectionLostSignal = false;
            /* fall through to backoff + retry - a fresh association means
             * the server forgot our prior RptEna/report-handler registration. */
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

    handle->stopRequested = false;
    handle->connectionLostSignal = false;
    handle->supervisorExited = false;
    handle->currentBackoffMs = 0;

    handle->supervisorThread = Thread_create(supervisorLoop, handle, false);
    if (!handle->supervisorThread) {
        Semaphore_destroy(handle->wakeSignal);
        handle->wakeSignal = NULL;
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
}
