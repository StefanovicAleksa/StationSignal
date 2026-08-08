#ifndef MMS_DATASET_MANAGER_UTILS_H_
#define MMS_DATASET_MANAGER_UTILS_H_

/*
 * Small string helper shared across this feature's data/ and domain/ layers.
 * Deliberately this feature's OWN copy rather than a shared cross-feature
 * one, matching the convention every other feature here already follows
 * (scl_bootstrap_utils, orchestration_utils, ied_discovery_utils and
 * mms_report_client_utils each carry their own) - see CLAUDE.md's Hard Rules
 * on this codebase having no precedent for a shared helper directory.
 */

/* NULL-safe strdup - returns NULL if s is NULL. Caller owns the result (free). */
char*
MmsDatasetManagerUtils_safeStringDup(const char* s);

#endif /* MMS_DATASET_MANAGER_UTILS_H_ */
