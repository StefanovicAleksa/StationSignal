#ifndef SCL_BOOTSTRAP_AUTH_H_
#define SCL_BOOTSTRAP_AUTH_H_

#include "iec61850_client.h"

/*
 * Isolates ACSE authentication wiring behind one function - auth is
 * negotiated at MMS association time, so this must be called on a fresh,
 * not-yet-connected IedConnection, before IedConnection_connect().
 *
 * Open question, deliberately not guessed: whether
 * IsoConnectionParameters_setAcseAuthenticationParameter takes ownership of
 * the AcseAuthenticationParameter (freed automatically when the owning
 * IedConnection is destroyed) is not documented at the setter in
 * iso_connection_parameters.h. This function does not free it itself; if
 * this leaks, it's bounded by the number of auth-retry attempts made during
 * one scan (at most one extra IedConnection per candidate), not by
 * long-running traffic - revisit with a valgrind run of the E2E test if this
 * becomes a real concern.
 */

/* NULL-safe (no-op if conn or password is NULL). password must outlive the
 * subsequent IedConnection_connect() call - the caller (scl_bootstrap_mms_session.c)
 * always passes SclBootstrapHandle's ownedAuthPassword, which is stable for
 * the handle's entire lifetime. */
void
SclBootstrapAuth_configurePasswordAuth(IedConnection conn, const char* password);

#endif /* SCL_BOOTSTRAP_AUTH_H_ */
