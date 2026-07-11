#include <stdlib.h>
#include "hal_thread.h"
#include "features/control_dispatcher/data/control_dispatcher_request_queue.h"

struct sControlDispatcherRequestQueue {
    ControlRequest** slots; /* owned circular array of capacity owned-or-NULL pointers */
    int capacity;
    int head;  /* index of the oldest unread item */
    int count;
    Semaphore lock;
};

ControlDispatcherRequestQueue
ControlDispatcherRequestQueue_create(int capacity) {
    if (capacity <= 0) return NULL;

    struct sControlDispatcherRequestQueue* queue = calloc(1, sizeof(struct sControlDispatcherRequestQueue));
    if (!queue) return NULL;

    queue->slots = calloc((size_t) capacity, sizeof(ControlRequest*));
    if (!queue->slots) {
        free(queue);
        return NULL;
    }

    queue->lock = Semaphore_create(1);
    if (!queue->lock) {
        free(queue->slots);
        free(queue);
        return NULL;
    }

    queue->capacity = capacity;
    return queue;
}

void
ControlDispatcherRequestQueue_destroy(ControlDispatcherRequestQueue queue) {
    if (!queue) return;

    for (int i = 0; i < queue->count; i++) {
        int index = (queue->head + i) % queue->capacity;
        ControlDispatcherRequest_destroy(queue->slots[index]);
    }
    free(queue->slots);
    if (queue->lock) Semaphore_destroy(queue->lock);
    free(queue);
}

bool
ControlDispatcherRequestQueue_push(ControlDispatcherRequestQueue queue, ControlRequest* request) {
    if (!queue || !request) return false;

    Semaphore_wait(queue->lock);

    if (queue->count >= queue->capacity) {
        Semaphore_post(queue->lock);
        return false;
    }

    int tail = (queue->head + queue->count) % queue->capacity;
    queue->slots[tail] = request;
    queue->count++;

    Semaphore_post(queue->lock);
    return true;
}

ControlRequest*
ControlDispatcherRequestQueue_pop(ControlDispatcherRequestQueue queue) {
    if (!queue) return NULL;

    Semaphore_wait(queue->lock);

    if (queue->count == 0) {
        Semaphore_post(queue->lock);
        return NULL;
    }

    ControlRequest* request = queue->slots[queue->head];
    queue->slots[queue->head] = NULL;
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    Semaphore_post(queue->lock);
    return request;
}

void
ControlDispatcherRequest_destroy(ControlRequest* request) {
    if (!request) return;

    free(request->requestId);
    free(request->host);
    free(request->iedName);
    free(request->interfaceId);
    free(request->sclFilePath);
    free(request->acseAuthPassword);
    free(request);
}
