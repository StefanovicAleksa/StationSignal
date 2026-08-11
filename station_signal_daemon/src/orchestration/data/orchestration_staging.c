#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "orchestration/data/orchestration_staging.h"
#include "log.h"

char*
OrchestrationStaging_writeTempFile(const uint8_t* fileData, uint32_t fileSize, int* outErrno) {
    if (!fileData) {
        if (outErrno) *outErrno = EINVAL;
        return NULL;
    }

    char* path = strdup("/tmp/orchestration_scl_XXXXXX");
    if (!path) {
        if (outErrno) *outErrno = ENOMEM;
        return NULL;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        if (outErrno) *outErrno = errno;
        free(path);
        return NULL;
    }

    uint32_t written = 0;
    while (written < fileSize) {
        ssize_t n = write(fd, fileData + written, fileSize - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (outErrno) *outErrno = errno;
            close(fd);
            unlink(path);
            free(path);
            return NULL;
        }
        written += (uint32_t) n;
    }

    if (close(fd) != 0) {
        if (outErrno) *outErrno = errno;
        unlink(path);
        free(path);
        return NULL;
    }

    if (outErrno) *outErrno = 0;
    return path;
}

void
OrchestrationStaging_cleanup(const char* path) {
    if (!path) return;

    if (unlink(path) != 0) {
        SS_LOG_WARN("[orchestration] warning: failed to remove staged temp file '%s': %s\n",
                path, strerror(errno));
    }
}
