#include <stdlib.h>
#include "features/scl_bootstrap/service/scl_bootstrap_api.h"
#include "features/scl_bootstrap/data/scl_bootstrap_tcp_probe.h"
#include "features/scl_bootstrap/data/scl_bootstrap_mms_session.h"
#include "features/scl_bootstrap/domain/scl_bootstrap_usecases.h"
#include "features/scl_bootstrap/utils/scl_bootstrap_utils.h"

void
SclBootstrapConfig_defaults(SclBootstrapConfig* config) {
    if (!config) return;

    config->tcpProbeTimeoutMs = 500;
    config->maxConcurrentTcpProbes = 16;
    config->mmsConnectTimeoutMs = 3000;
    config->mmsRequestTimeoutMs = 0;
    config->maxBrowseDepth = 2;
    config->acseAuthPassword = NULL;
}

SclBootstrapHandle
SclBootstrap_create(const SclBootstrapConfig* config, SclBootstrapError* outError) {
    SclBootstrapHandle handle = calloc(1, sizeof(struct sSclBootstrapHandle));
    if (!handle) {
        if (outError) *outError = SCL_BOOTSTRAP_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config) {
        handle->config = *config;
    } else {
        SclBootstrapConfig_defaults(&handle->config);
    }

    /* Own a stable copy of the password, independent of whatever buffer the
     * caller's config struct originally pointed at - see scl_bootstrap_types.h. */
    handle->ownedAuthPassword = SclBootstrapUtils_safeStringDup(handle->config.acseAuthPassword);
    handle->config.acseAuthPassword = handle->ownedAuthPassword;

    if (outError) *outError = SCL_BOOTSTRAP_OK;
    return handle;
}

void
SclBootstrap_setProgressCallback(SclBootstrapHandle handle,
        SclBootstrapProgressCallback callback, void* userParam) {
    if (!handle) return;
    handle->progressCallback = callback;
    handle->progressCallbackParam = userParam;
}

LinkedList
SclBootstrap_scanAndFetch(SclBootstrapHandle handle, LinkedList hostList, int mmsPort,
        SclBootstrapError* outError) {
    if (!handle || !SclBootstrapUseCases_isHostListValid(hostList) || mmsPort <= 0) {
        if (outError) *outError = SCL_BOOTSTRAP_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    bool* reachable = SclBootstrapTcpProbe_scan(hostList, mmsPort,
            handle->config.tcpProbeTimeoutMs, handle->config.maxConcurrentTcpProbes);
    if (!reachable) {
        if (outError) *outError = SCL_BOOTSTRAP_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    LinkedList results = LinkedList_create();
    if (!results) {
        free(reachable);
        if (outError) *outError = SCL_BOOTSTRAP_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    int index = 0;
    LinkedList element = LinkedList_getNext(hostList);
    while (element) {
        const char* host = (const char*) LinkedList_getData(element);

        SclBootstrapResult* result = calloc(1, sizeof(SclBootstrapResult));
        if (result) {
            result->host = SclBootstrapUtils_safeStringDup(host);
            result->port = mmsPort;
            result->lastMmsError = IED_ERROR_OK;

            if (!reachable[index]) {
                result->status = SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER;
            } else {
                SclBootstrapMmsSession_run(result, &handle->config, handle->ownedAuthPassword);
            }

            LinkedList_add(results, result);

            if (handle->progressCallback) {
                handle->progressCallback(handle->progressCallbackParam, result);
            }
        }

        element = LinkedList_getNext(element);
        index++;
    }

    free(reachable);
    if (outError) *outError = SCL_BOOTSTRAP_OK;
    return results;
}

void
SclBootstrap_destroyResult(void* resultPtr) {
    SclBootstrapResult* result = (SclBootstrapResult*) resultPtr;
    if (!result) return;

    free(result->host);
    free(result->fileName);
    free(result->fileData);
    free(result);
}

void
SclBootstrap_destroy(SclBootstrapHandle handle) {
    if (!handle) return;

    free(handle->ownedAuthPassword);
    free(handle);
}

bool*
SclBootstrap_tcpProbeOnly(SclBootstrapHandle handle, LinkedList hostList, int mmsPort) {
    if (!handle || !SclBootstrapUseCases_isHostListValid(hostList) || mmsPort <= 0) {
        return NULL;
    }

    return SclBootstrapTcpProbe_scan(hostList, mmsPort,
            handle->config.tcpProbeTimeoutMs, handle->config.maxConcurrentTcpProbes);
}
