#include <stdlib.h>
#include <string.h>
#include "hal_thread.h"
#include "features/scan_dispatcher/data/scan_dispatcher_ring_buffer.h"

struct sScanDispatcherRingBuffer {
    char** slots;    /* owned array of capacity owned-or-NULL JSON strings */
    int capacity;
    uint64_t head;   /* total messages ever pushed == next sequence number to write */
    Semaphore lock;
};

ScanDispatcherRingBuffer
ScanDispatcherRingBuffer_create(int capacity) {
    if (capacity <= 0) return NULL;

    struct sScanDispatcherRingBuffer* buffer = calloc(1, sizeof(struct sScanDispatcherRingBuffer));
    if (!buffer) return NULL;

    buffer->slots = calloc((size_t) capacity, sizeof(char*));
    if (!buffer->slots) {
        free(buffer);
        return NULL;
    }

    buffer->lock = Semaphore_create(1);
    if (!buffer->lock) {
        free(buffer->slots);
        free(buffer);
        return NULL;
    }

    buffer->capacity = capacity;
    buffer->head = 0;

    return buffer;
}

void
ScanDispatcherRingBuffer_destroy(ScanDispatcherRingBuffer buffer) {
    if (!buffer) return;

    for (int i = 0; i < buffer->capacity; i++) free(buffer->slots[i]);
    free(buffer->slots);
    if (buffer->lock) Semaphore_destroy(buffer->lock);
    free(buffer);
}

void
ScanDispatcherRingBuffer_push(ScanDispatcherRingBuffer buffer, char* json) {
    if (!buffer) { free(json); return; }

    Semaphore_wait(buffer->lock);

    int slot = (int) (buffer->head % (uint64_t) buffer->capacity);
    free(buffer->slots[slot]);
    buffer->slots[slot] = json;
    buffer->head++;

    Semaphore_post(buffer->lock);
}

char*
ScanDispatcherRingBuffer_readNext(ScanDispatcherRingBuffer buffer, uint64_t* cursor, uint64_t* outDroppedForThisRead) {
    if (outDroppedForThisRead) *outDroppedForThisRead = 0;
    if (!buffer || !cursor) return NULL;

    Semaphore_wait(buffer->lock);

    uint64_t head = buffer->head;
    if (*cursor >= head) {
        Semaphore_post(buffer->lock);
        return NULL;
    }

    uint64_t oldestAvailable = (head > (uint64_t) buffer->capacity) ? (head - (uint64_t) buffer->capacity) : 0;
    if (*cursor < oldestAvailable) {
        if (outDroppedForThisRead) *outDroppedForThisRead = oldestAvailable - *cursor;
        *cursor = oldestAvailable;
    }

    int slot = (int) (*cursor % (uint64_t) buffer->capacity);
    const char* slotValue = buffer->slots[slot];
    char* copy = NULL;
    if (slotValue) {
        size_t len = strlen(slotValue) + 1;
        copy = malloc(len);
        if (copy) memcpy(copy, slotValue, len);
    }
    (*cursor)++;

    Semaphore_post(buffer->lock);

    return copy;
}

uint64_t
ScanDispatcherRingBuffer_headSeq(ScanDispatcherRingBuffer buffer) {
    if (!buffer) return 0;

    Semaphore_wait(buffer->lock);
    uint64_t head = buffer->head;
    Semaphore_post(buffer->lock);

    return head;
}
