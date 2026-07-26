#ifndef IED_DISCOVERY_AUTH_H_
#define IED_DISCOVERY_AUTH_H_

#include "iec61850_client.h"

/*
 * Configures ACSE password auth on conn before connecting. Deliberate
 * duplicate of scl_bootstrap_auth.c's SclBootstrapAuth_configurePasswordAuth
 * (itself already duplicated once, into mms_report_client_auth.c) - features
 * never reach into another feature's data/domain layers, only their public
 * service header, and this snippet is small enough to match that
 * established precedent rather than warranting a new shared entry point.
 */
void
IedDiscoveryAuth_configurePasswordAuth(IedConnection conn, const char* password);

#endif /* IED_DISCOVERY_AUTH_H_ */
