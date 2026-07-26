#ifndef ORCHESTRATION_USECASES_H_
#define ORCHESTRATION_USECASES_H_

#include "features/scl_bootstrap/service/scl_bootstrap_api.h"

/*
 * Pure sequencing/selection logic over SclBootstrap_scanAndFetch's result
 * list - no I/O, no libiec61850 symbols beyond LinkedList itself, same
 * posture as scl_bootstrap's own domain/scl_bootstrap_usecases.c.
 */

/*
 * Scans results in input order for the first element whose .status ==
 * SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED, detaches it from the list via
 * LinkedList_remove (does NOT free it) and returns it - so it survives a
 * subsequent LinkedList_destroyDeep(results, SclBootstrap_destroyResult) of
 * whatever remains. Returns NULL if no candidate reached FILE_RETRIEVED;
 * results is left fully intact in that case. Caller owns the returned
 * result (SclBootstrap_destroyResult it when done).
 */
SclBootstrapResult*
OrchestrationUseCases_selectAndDetachFirstRetrieved(LinkedList results);

/*
 * Returns the .status of the last element in results - a cheap, best-effort
 * diagnostic for OrchestrationErrorDetail.lastCandidateStatus when no
 * candidate reached FILE_RETRIEVED. Returns SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER
 * if results is empty (defensive - scanAndFetch never actually returns an
 * empty non-NULL list for a non-empty hostList, but this keeps the helper total).
 */
SclBootstrapCandidateStatus
OrchestrationUseCases_summarizeBootstrapFailure(LinkedList results);

#endif /* ORCHESTRATION_USECASES_H_ */
