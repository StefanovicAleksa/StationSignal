#include <stdlib.h>
#include <string.h>
#include "hal_thread.h"
#include "linked_list.h"
#include "scan_orchestration/data/scan_orchestration_worker.h"
#include "scan_orchestration/domain/scan_orchestration_usecases.h"
#include "features/scan_dispatcher/service/scan_dispatcher_api.h"

struct sScanOrchestrationWorker {
    uint64_t scanId;
    char* interfaceId;              /* owned copy */
    int mmsPort;
    uint32_t sweepIntervalMs;
    char* ownedAcseAuthPassword;    /* owned copy or NULL */

    IedDiscoveryHandle discoveryHandle;   /* owned, private to this scan */
    ScanDispatcherHandle scanDispatcher;  /* borrowed */

    ScanOrchestrationDeviceFoundCallback foundCallback;  /* borrowed, may be NULL */
    void* foundCallbackParam;

    Semaphore seenSetLock;   /* hal_thread.h Semaphore(1) as binary mutex - same convention as
                                goose_subscriber's targetStateLock / ipc_dispatcher_ring_buffer's own lock */
    char** seenHosts;        /* owned array of owned strings */
    int seenCount;
    int seenCapacity;

    Thread thread;
    volatile bool stopRequested;
    volatile bool exited;
};

static char*
safeStringDup(const char* s) {
    return s ? strdup(s) : NULL;
}

static bool
appendToSeenSet(struct sScanOrchestrationWorker* worker, const char* host) {
    if (worker->seenCount >= worker->seenCapacity) {
        int newCapacity = (worker->seenCapacity > 0) ? (worker->seenCapacity * 2) : 16;
        char** grown = realloc(worker->seenHosts, (size_t) newCapacity * sizeof(char*));
        if (!grown) return false;
        worker->seenHosts = grown;
        worker->seenCapacity = newCapacity;
    }

    char* copy = strdup(host);
    if (!copy) return false;

    worker->seenHosts[worker->seenCount++] = copy;
    return true;
}

/* Sleeps in small chunks so ScanOrchestrationWorker_stop's bounded wait for
 * the sweep loop to notice stopRequested doesn't have to wait out a full
 * sweepIntervalMs - same idiom as goose_subscriber_connection.c's own
 * interruptibleSleep (hal_thread.h has no interruptible sleep primitive). */
static void
interruptibleSleep(struct sScanOrchestrationWorker* worker, uint32_t totalMs) {
    const uint32_t chunkMs = 20;
    uint32_t slept = 0;
    while (slept < totalMs && !worker->stopRequested) {
        uint32_t thisChunk = (totalMs - slept) < chunkMs ? (totalMs - slept) : chunkMs;
        Thread_sleep((int) thisChunk);
        slept += thisChunk;
    }
}

/* scan_dispatcher only binds on this scan's 0->1 transition, so the client can't have a live
 * connection to it before START_SCAN's ack arrives - it force-reconnects only after receiving
 * that ack (see ws_test_scan.html's connectScanWs(force) comment). Both dispatcher ws_servers use
 * "start-from-now" ring-buffer cursors (no backlog replay - see CLAUDE.md), so if the first sweep
 * below publishes a found host before that reconnect's WS handshake completes, the event is lost
 * to that client forever (ScanOrchestrationUseCases_isHostNew dedup means it won't be republished
 * on a later sweep) - reproducible every time against a fast/local responder. This grace delay
 * only runs before the very first sweep, giving a well-behaved client's reconnect (typically
 * single-digit ms on loopback) time to land first. */
#define SCAN_ORCHESTRATION_INITIAL_SWEEP_GRACE_MS 300

static void*
sweepLoop(void* parameter) {
    struct sScanOrchestrationWorker* worker = (struct sScanOrchestrationWorker*) parameter;

    interruptibleSleep(worker, SCAN_ORCHESTRATION_INITIAL_SWEEP_GRACE_MS);

    while (!worker->stopRequested) {
        IedDiscoveryError err;
        LinkedList results = IedDiscovery_scanSubnet(worker->discoveryHandle, worker->interfaceId,
                worker->mmsPort, &err);
        /* NULL results (bad interface / subnet too large / OOM) tolerated
         * gracefully - skip to the sleep below, don't crash/exit the loop.
         * Matches this codebase's existing tolerance for a single failed
         * round (e.g. goose_subscriber's liveness loop tolerating a missed
         * poll). */

        if (results) {
            LinkedList el = LinkedList_getNext(results);
            while (el && !worker->stopRequested) {
                const char* host = (const char*) LinkedList_getData(el);

                Semaphore_wait(worker->seenSetLock);
                bool isNew = ScanOrchestrationUseCases_isHostNew(
                        (const char* const*) worker->seenHosts, worker->seenCount, host);
                if (isNew) appendToSeenSet(worker, host);
                Semaphore_post(worker->seenSetLock);

                /* Publish/callback OUTSIDE the lock - same pattern
                 * goose_subscriber_connection.c's livenessLoop already
                 * establishes. Also re-checks stopRequested right before
                 * publishing, mirroring the enableAllTargets bugfix
                 * documented in CLAUDE.md ("check stopRequested once per
                 * loop iteration") - avoids delivering a found event after a
                 * stop was already requested mid-sweep. */
                if (isNew && !worker->stopRequested) {
                    ScanDispatcher_publishDeviceFound(worker->scanDispatcher, worker->scanId, host, worker->mmsPort);
                    if (worker->foundCallback) {
                        worker->foundCallback(worker->foundCallbackParam, worker->scanId, host, worker->mmsPort);
                    }
                }

                el = LinkedList_getNext(el);
            }
            LinkedList_destroyDeep(results, free);
        }

        interruptibleSleep(worker, worker->sweepIntervalMs);
    }

    worker->exited = true;
    return NULL;
}

ScanOrchestrationWorker
ScanOrchestrationWorker_create(uint64_t scanId, const ScanRequest* request,
        const IedDiscoveryConfig* discoveryConfigTemplate, uint32_t defaultSweepIntervalMs,
        ScanDispatcherHandle scanDispatcher,
        ScanOrchestrationDeviceFoundCallback foundCallback, void* foundCallbackParam,
        ScanOrchestrationError* outError) {
    if (!request || !request->interfaceId || !request->interfaceId[0] || request->mmsPort <= 0
            || !discoveryConfigTemplate) {
        if (outError) *outError = SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    struct sScanOrchestrationWorker* worker = calloc(1, sizeof(struct sScanOrchestrationWorker));
    if (!worker) {
        if (outError) *outError = SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    worker->scanId = scanId;
    worker->interfaceId = strdup(request->interfaceId);
    worker->mmsPort = request->mmsPort;
    worker->sweepIntervalMs = (request->sweepIntervalMs > 0) ? request->sweepIntervalMs : defaultSweepIntervalMs;
    worker->ownedAcseAuthPassword = safeStringDup(request->acseAuthPassword);
    worker->scanDispatcher = scanDispatcher;
    worker->foundCallback = foundCallback;
    worker->foundCallbackParam = foundCallbackParam;

    if (!worker->interfaceId) {
        free(worker->interfaceId);
        free(worker->ownedAcseAuthPassword);
        free(worker);
        if (outError) *outError = SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    worker->seenSetLock = Semaphore_create(1);
    if (!worker->seenSetLock) {
        free(worker->interfaceId);
        free(worker->ownedAcseAuthPassword);
        free(worker);
        if (outError) *outError = SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    IedDiscoveryConfig discoveryConfig = *discoveryConfigTemplate;
    discoveryConfig.acseAuthPassword = worker->ownedAcseAuthPassword;

    IedDiscoveryError discErr;
    worker->discoveryHandle = IedDiscovery_create(&discoveryConfig, &discErr);
    if (!worker->discoveryHandle) {
        Semaphore_destroy(worker->seenSetLock);
        free(worker->interfaceId);
        free(worker->ownedAcseAuthPassword);
        free(worker);
        if (outError) *outError = SCAN_ORCHESTRATION_ERR_DISCOVERY_CREATE_FAILED;
        return NULL;
    }

    if (outError) *outError = SCAN_ORCHESTRATION_OK;
    return worker;
}

ScanOrchestrationError
ScanOrchestrationWorker_start(ScanOrchestrationWorker worker) {
    if (!worker) return SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT;

    worker->stopRequested = false;
    worker->exited = false;

    worker->thread = Thread_create(sweepLoop, worker, false);
    if (!worker->thread) return SCAN_ORCHESTRATION_ERR_THREAD_CREATE_FAILED;

    Thread_start(worker->thread);
    return SCAN_ORCHESTRATION_OK;
}

void
ScanOrchestrationWorker_stop(ScanOrchestrationWorker worker) {
    if (!worker || worker->stopRequested) return;

    worker->stopRequested = true;

    if (worker->thread) {
        while (!worker->exited) Thread_sleep(20);
    }
}

void
ScanOrchestrationWorker_destroy(ScanOrchestrationWorker worker) {
    if (!worker) return;

    ScanOrchestrationWorker_stop(worker); /* idempotent - safe even if never started */

    if (worker->thread) {
        Thread_destroy(worker->thread);
        worker->thread = NULL;
    }
    if (worker->discoveryHandle) {
        IedDiscovery_destroy(worker->discoveryHandle);
        worker->discoveryHandle = NULL;
    }
    if (worker->seenSetLock) {
        Semaphore_destroy(worker->seenSetLock);
        worker->seenSetLock = NULL;
    }
    for (int i = 0; i < worker->seenCount; i++) free(worker->seenHosts[i]);
    free(worker->seenHosts);
    free(worker->interfaceId);
    free(worker->ownedAcseAuthPassword);
    free(worker);
}

uint64_t
ScanOrchestrationWorker_scanId(ScanOrchestrationWorker worker) {
    return worker ? worker->scanId : 0;
}

char**
ScanOrchestrationWorker_snapshotHosts(ScanOrchestrationWorker worker, int* outCount) {
    if (outCount) *outCount = 0;
    if (!worker) return NULL;

    Semaphore_wait(worker->seenSetLock);
    int count = worker->seenCount;
    char** snapshot = (count > 0) ? malloc((size_t) count * sizeof(char*)) : NULL;
    if (snapshot) {
        for (int i = 0; i < count; i++) snapshot[i] = strdup(worker->seenHosts[i]);
    } else {
        count = 0;
    }
    Semaphore_post(worker->seenSetLock);

    if (outCount) *outCount = count;
    return snapshot;
}

void
ScanOrchestrationWorker_freeSnapshot(char** hosts, int count) {
    if (!hosts) return;
    for (int i = 0; i < count; i++) free(hosts[i]);
    free(hosts);
}
