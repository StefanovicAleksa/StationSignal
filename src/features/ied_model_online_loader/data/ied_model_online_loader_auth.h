#ifndef IED_MODEL_ONLINE_LOADER_AUTH_H_
#define IED_MODEL_ONLINE_LOADER_AUTH_H_

#include "iec61850_client.h"

/*
 * Configures ACSE password authentication on `conn` before connect - no-op if
 * password is NULL. Mirrors scl_bootstrap's data/scl_bootstrap_auth.c and
 * mms_report_client's data/mms_report_client_auth.c (identical snippet),
 * duplicated rather than shared - features never reach into each other's
 * data/domain layers, only service headers (same convention those two
 * features' own doc comments already state for each other).
 */
void
IedModelOnlineLoaderAuth_configurePasswordAuth(IedConnection conn, const char* password);

#endif /* IED_MODEL_ONLINE_LOADER_AUTH_H_ */
