#ifndef ORCHESTRATION_STAGING_H_
#define ORCHESTRATION_STAGING_H_

#include <stdint.h>

/*
 * Bridges scl_bootstrap's in-memory SCL bytes (SclBootstrapResult.fileData/
 * fileSize) to ied_model's file-path-only IedModel_loadFromFile - deliberately
 * decoupled from every other orchestration type (no LinkedList/IedModelHandle
 * dependency), so this stays trivially unit-testable in isolation.
 */

/*
 * Writes fileData/fileSize to a new mkstemp-created file under /tmp. Returns
 * an owned, heap-allocated path string (free() it) on success, NULL on
 * failure (outErrno set to the errno of whichever syscall failed:
 * mkstemp/write/close).
 */
char*
OrchestrationStaging_writeTempFile(const uint8_t* fileData, uint32_t fileSize, int* outErrno);

/*
 * Best-effort unlink; logs to stderr on failure but never treated as fatal
 * by callers (the file being briefly orphaned in /tmp is a nuisance, not a
 * correctness issue). NULL-safe.
 */
void
OrchestrationStaging_cleanup(const char* path);

#endif /* ORCHESTRATION_STAGING_H_ */
