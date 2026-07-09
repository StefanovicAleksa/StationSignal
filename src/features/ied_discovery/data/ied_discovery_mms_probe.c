#include "features/ied_discovery/data/ied_discovery_mms_probe.h"
#include "features/ied_discovery/data/ied_discovery_auth.h"
#include "iec61850_client.h"

/*
 * One connect attempt, immediately closed on success. *outAccessDenied
 * signals whether the specific failure looked like an ACSE-level rejection
 * (same heuristic as scl_bootstrap_mms_session.c's runSequenceOnce) - the
 * caller uses this to decide whether an auth retry might help.
 */
static bool
tryAssociateOnce(const char* host, int port, uint32_t connectTimeoutMs, const char* password,
        bool* outAccessDenied) {
    *outAccessDenied = false;

    IedConnection conn = IedConnection_create();
    if (!conn) return false;

    if (password) IedDiscoveryAuth_configurePasswordAuth(conn, password);
    if (connectTimeoutMs > 0) IedConnection_setConnectTimeout(conn, connectTimeoutMs);

    IedClientError err = IED_ERROR_OK;
    IedConnection_connect(conn, &err, host, port);

    if (err != IED_ERROR_OK) {
        *outAccessDenied = (err == IED_ERROR_ACCESS_DENIED || err == IED_ERROR_CONNECTION_REJECTED);
        IedConnection_destroy(conn);
        return false;
    }

    IedConnection_close(conn);
    IedConnection_destroy(conn);
    return true;
}

bool
IedDiscoveryMmsProbe_associate(const char* host, int port, uint32_t connectTimeoutMs,
        const char* ownedAuthPassword) {
    bool accessDenied = false;
    if (tryAssociateOnce(host, port, connectTimeoutMs, NULL, &accessDenied)) return true;

    if (accessDenied && ownedAuthPassword) {
        bool retryAccessDenied = false;
        return tryAssociateOnce(host, port, connectTimeoutMs, ownedAuthPassword, &retryAccessDenied);
    }

    return false;
}
