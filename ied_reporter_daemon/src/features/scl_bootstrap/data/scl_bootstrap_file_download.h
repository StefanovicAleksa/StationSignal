#ifndef SCL_BOOTSTRAP_FILE_DOWNLOAD_H_
#define SCL_BOOTSTRAP_FILE_DOWNLOAD_H_

#include <stdint.h>
#include "iec61850_client.h"

/*
 * Growable in-memory accumulator for IedConnection_getFile's
 * IedClientGetFileHandler callback. The header's own doc comment requires the
 * handler to copy the chunk to another location before returning - this is
 * that other location.
 */
typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
} SclBootstrapFileDownloadBuffer;

void
SclBootstrapFileDownloadBuffer_init(SclBootstrapFileDownloadBuffer* buffer);

/* Frees buffer->data. NULL-safe. Does not free the buffer struct itself
 * (typically stack-allocated by the caller). */
void
SclBootstrapFileDownloadBuffer_free(SclBootstrapFileDownloadBuffer* buffer);

/*
 * IedClientGetFileHandler-compatible: parameter must be a
 * SclBootstrapFileDownloadBuffer*. Appends chunk to the buffer, growing it as
 * needed. Always returns true (continue) unless a chunk can't be copied due
 * to allocation failure, in which case it returns false to stop the transfer.
 */
bool
SclBootstrapFileDownload_handler(void* parameter, uint8_t* chunk, uint32_t bytesRead);

#endif /* SCL_BOOTSTRAP_FILE_DOWNLOAD_H_ */
