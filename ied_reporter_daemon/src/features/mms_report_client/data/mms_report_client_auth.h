#ifndef MMS_REPORT_CLIENT_AUTH_H_
#define MMS_REPORT_CLIENT_AUTH_H_

#include "iec61850_client.h"

/*
 * Isolates ACSE authentication wiring behind one function - same convention
 * and same unresolved-ownership caveat as scl_bootstrap_auth.h (see that
 * file's own doc comment: whether IsoConnectionParameters_setAcseAuthenticationParameter
 * takes ownership of the AcseAuthenticationParameter is not documented in
 * iso_connection_parameters.h). Auth is negotiated at MMS association time,
 * so this must be called on a fresh, not-yet-connected IedConnection,
 * before its first IedConnection_connect().
 *
 * Unlike scl_bootstrap (which creates a fresh IedConnection per candidate/
 * attempt), mms_report_client reuses the SAME IedConnection object across
 * every reconnect attempt (see mms_report_client_connection.c's supervisor
 * loop) - so calling this once, at MmsReportClientConnection_create time,
 * before the connection is ever used, covers every subsequent reconnect
 * too. No per-attempt retry logic is needed here the way scl_bootstrap
 * needs one: mms_report_client always targets one already-known IED (not a
 * blind multi-candidate scan), so if a password is configured at all, it's
 * applied unconditionally from the very first attempt.
 */

/* NULL-safe (no-op if conn or password is NULL). password must outlive the
 * connection - the caller (mms_report_client_connection.c) always passes
 * MmsReportClientHandle's ownedAuthPassword, which is stable for the
 * handle's entire lifetime. */
void
MmsReportClientAuth_configurePasswordAuth(IedConnection conn, const char* password);

#endif /* MMS_REPORT_CLIENT_AUTH_H_ */
