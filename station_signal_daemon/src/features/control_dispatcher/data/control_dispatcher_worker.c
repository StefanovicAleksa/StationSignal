#define SS_LOG_FEATURE "control_dispatcher"
#include <stdio.h>
#include <stdlib.h>
#include "hal_thread.h"
#include "hal_time.h"
#include "features/control_dispatcher/data/control_dispatcher_worker.h"
#include "features/control_dispatcher/domain/control_dispatcher_usecases.h"
#include "log.h"

struct sControlDispatcherWorker {
    ControlDispatcherRequestQueue queue;   /* borrowed */
    ControlDispatcherRingBuffer ringBuffer; /* borrowed */
    ControlDispatcherWsServer wsServer;     /* borrowed */
    DeviceManagerHandle deviceManager;      /* borrowed */
    ScanOrchestrationHandle scanOrchestration; /* borrowed */

    Semaphore wakeSignal; /* Semaphore_create(0) - posted once per queued request, plus once
                              more on stop to guarantee a prompt exit even if idle */
    volatile bool stopRequested;
    volatile bool exited;
    Thread thread;
};

static void*
workerLoop(void* parameter) {
    struct sControlDispatcherWorker* worker = (struct sControlDispatcherWorker*) parameter;

    while (!worker->stopRequested) {
        Semaphore_wait(worker->wakeSignal);
        if (worker->stopRequested) break;

        ControlRequest* request = ControlDispatcherRequestQueue_pop(worker->queue);
        if (!request) continue; /* spurious wake (or a racing stop) - tolerated */

        /* THE SLOW PART - the only place in this feature that may block for
         * seconds (SCL bootstrap over the network, MMS connect, or a scan's
         * own synchronous first-sweep dispatch). */
        uint64_t requestStartMs = Hal_getTimeInMs();
        char* json = ControlDispatcherUseCases_processRequest(request, worker->deviceManager,
                worker->scanOrchestration);
        SS_LOG_DEBUG("[control_dispatcher] request %s type=%d done (%llums)\n", request->requestId,
                (int) request->type, (unsigned long long) (Hal_getTimeInMs() - requestStartMs));
        ControlDispatcherRequest_destroy(request);

        if (json) {
            ControlDispatcherRingBuffer_push(worker->ringBuffer, json); /* ownership -> ring buffer */
            ControlDispatcherWsServer_wake(worker->wsServer);
        }
    }

    worker->exited = true;
    return NULL;
}

ControlDispatcherWorker
ControlDispatcherWorker_create(ControlDispatcherRequestQueue queue, ControlDispatcherRingBuffer ringBuffer,
        ControlDispatcherWsServer wsServer, DeviceManagerHandle deviceManager,
        ScanOrchestrationHandle scanOrchestration, ControlDispatcherError* outError) {
    if (!queue || !ringBuffer || !wsServer || !deviceManager || !scanOrchestration) {
        if (outError) *outError = CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    struct sControlDispatcherWorker* worker = calloc(1, sizeof(struct sControlDispatcherWorker));
    if (!worker) {
        if (outError) *outError = CONTROL_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    worker->wakeSignal = Semaphore_create(0);
    if (!worker->wakeSignal) {
        free(worker);
        if (outError) *outError = CONTROL_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    worker->queue = queue;
    worker->ringBuffer = ringBuffer;
    worker->wsServer = wsServer;
    worker->deviceManager = deviceManager;
    worker->scanOrchestration = scanOrchestration;

    if (outError) *outError = CONTROL_DISPATCHER_OK;
    return worker;
}

ControlDispatcherError
ControlDispatcherWorker_start(ControlDispatcherWorker worker) {
    if (!worker) return CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT;

    worker->stopRequested = false;
    worker->exited = false;

    worker->thread = Thread_create(workerLoop, worker, false);
    if (!worker->thread) return CONTROL_DISPATCHER_ERR_THREAD_CREATE_FAILED;

    Thread_start(worker->thread);
    return CONTROL_DISPATCHER_OK;
}

void
ControlDispatcherWorker_notify(ControlDispatcherWorker worker) {
    if (!worker) return;
    Semaphore_post(worker->wakeSignal);
}

void
ControlDispatcherWorker_stop(ControlDispatcherWorker worker) {
    if (!worker || worker->stopRequested) return;

    worker->stopRequested = true;
    Semaphore_post(worker->wakeSignal); /* prompt exit even if idle */

    if (worker->thread) {
        while (!worker->exited) Thread_sleep(20);
    }
}

void
ControlDispatcherWorker_destroy(ControlDispatcherWorker worker) {
    if (!worker) return;

    ControlDispatcherWorker_stop(worker);

    if (worker->thread) {
        Thread_destroy(worker->thread);
        worker->thread = NULL;
    }
    if (worker->wakeSignal) {
        Semaphore_destroy(worker->wakeSignal);
        worker->wakeSignal = NULL;
    }
    free(worker);
}
