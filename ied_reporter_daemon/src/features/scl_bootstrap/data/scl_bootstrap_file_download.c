#include <stdlib.h>
#include <string.h>
#include "features/scl_bootstrap/data/scl_bootstrap_file_download.h"

void
SclBootstrapFileDownloadBuffer_init(SclBootstrapFileDownloadBuffer* buffer) {
    if (!buffer) return;
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

void
SclBootstrapFileDownloadBuffer_free(SclBootstrapFileDownloadBuffer* buffer) {
    if (!buffer) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

bool
SclBootstrapFileDownload_handler(void* parameter, uint8_t* chunk, uint32_t bytesRead) {
    SclBootstrapFileDownloadBuffer* buffer = (SclBootstrapFileDownloadBuffer*) parameter;
    if (bytesRead == 0) return true;

    uint32_t newSize = buffer->size + bytesRead;
    if (newSize > buffer->capacity) {
        uint8_t* newData = realloc(buffer->data, newSize);
        if (!newData) return false;
        buffer->data = newData;
        buffer->capacity = newSize;
    }

    memcpy(buffer->data + buffer->size, chunk, bytesRead);
    buffer->size = newSize;
    return true;
}
