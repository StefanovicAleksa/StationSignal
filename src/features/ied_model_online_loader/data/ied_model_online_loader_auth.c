#include "features/ied_model_online_loader/data/ied_model_online_loader_auth.h"
#include "mms_client_connection.h"
#include "iso_connection_parameters.h"

void
IedModelOnlineLoaderAuth_configurePasswordAuth(IedConnection conn, const char* password) {
    if (!conn || !password) return;

    MmsConnection mmsConn = IedConnection_getMmsConnection(conn);
    if (!mmsConn) return;

    IsoConnectionParameters isoParams = MmsConnection_getIsoConnectionParameters(mmsConn);
    if (!isoParams) return;

    AcseAuthenticationParameter authParam = AcseAuthenticationParameter_create();
    if (!authParam) return;

    AcseAuthenticationParameter_setAuthMechanism(authParam, ACSE_AUTH_PASSWORD);
    AcseAuthenticationParameter_setPassword(authParam, (char*) password);
    IsoConnectionParameters_setAcseAuthenticationParameter(isoParams, authParam);
}
