#ifndef ORCHESTRATION_UTILS_H_
#define ORCHESTRATION_UTILS_H_

#include "orchestration/domain/orchestration_types.h"

/* NULL-safe strdup - returns NULL if s is NULL. Caller owns the result (free). */
char*
OrchestrationUtils_safeStringDup(const char* s);

/*
 * Human-readable description of an OrchestrationError, for logging only
 * (never for control flow). Exists so callers like main.c's error-path
 * printf stay a one-liner. Always returns a non-NULL string.
 */
const char*
OrchestrationUtils_errorToString(OrchestrationError err);

#endif /* ORCHESTRATION_UTILS_H_ */
