#ifndef SCL_BOOTSTRAP_MMS_SESSION_H_
#define SCL_BOOTSTRAP_MMS_SESSION_H_

#include "features/scl_bootstrap/domain/scl_bootstrap_types.h"

/*
 * Phase 2 of scanning: the full per-candidate MMS lifecycle - connect, browse
 * the file directory tree (bounded by config->maxBrowseDepth), pick one SCL
 * file by extension priority, download it. Deliberately not unit-tested - a
 * live IedConnection can't be meaningfully faked in a hermetic unit test,
 * matching ied_model's scl_loader / mms_report_client's connection.c
 * convention of proving this kind of code E2E instead.
 *
 * Never leaves an IedConnection open on return, regardless of outcome.
 */

/*
 * result->host/port must already be set by the caller (this is filled in
 * once by the service layer from the original candidate list, before phase 2
 * runs). Fills in status/lastMmsError/authWasAttempted, and on success
 * fileName/fileData/fileSize.
 *
 * If the first attempt (no auth) fails with access-denied at any stage
 * (connect/browse/download) and ownedAuthPassword is non-NULL, retries the
 * whole sequence exactly once on a fresh connection with ACSE password auth
 * configured (see scl_bootstrap_auth.h) before giving up.
 */
void
SclBootstrapMmsSession_run(SclBootstrapResult* result, const SclBootstrapConfig* config,
        const char* ownedAuthPassword);

#endif /* SCL_BOOTSTRAP_MMS_SESSION_H_ */
