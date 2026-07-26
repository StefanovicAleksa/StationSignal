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

/*
 * Human-readable description of an OrchestrationStage, for logging only.
 * Always returns a non-NULL string.
 */
const char*
OrchestrationUtils_stageToString(OrchestrationStage stage);

/*
 * Human-readable description of a SclBootstrapCandidateStatus, for logging
 * only - explains *why* the BOOTSTRAP stage failed (OrchestrationErrorDetail's
 * generic stage/error alone only say *which* stage failed and *that*
 * scl_bootstrap failed, not the specific candidate-level reason). Extracted
 * from main.c's own original private helper of the same shape (see CLAUDE.md's
 * main.c Current-State bullet, pre-device_manager version) so
 * control_dispatcher's control-plane error-mapping and main.c's own
 * diagnostics share one copy instead of duplicating the string table. Always
 * returns a non-NULL string.
 */
const char*
OrchestrationUtils_candidateStatusToString(SclBootstrapCandidateStatus status);

#endif /* ORCHESTRATION_UTILS_H_ */
