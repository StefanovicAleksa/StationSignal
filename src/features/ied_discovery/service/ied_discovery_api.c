#include <stdlib.h>
#include "features/ied_discovery/service/ied_discovery_api.h"
#include "features/ied_discovery/domain/ied_discovery_cidr.h"
#include "features/ied_discovery/data/ied_discovery_netif.h"
#include "features/ied_discovery/data/ied_discovery_mms_probe.h"
#include "features/ied_discovery/utils/ied_discovery_utils.h"
#include "features/scl_bootstrap/service/scl_bootstrap_api.h"

void
IedDiscoveryConfig_defaults(IedDiscoveryConfig* config) {
    if (!config) return;

    config->tcpProbeTimeoutMs = 500;
    config->maxConcurrentTcpProbes = 64;
    config->mmsConnectTimeoutMs = 3000;
    config->maxHosts = 1024;
    config->acseAuthPassword = NULL;
}

IedDiscoveryHandle
IedDiscovery_create(const IedDiscoveryConfig* config, IedDiscoveryError* outError) {
    IedDiscoveryHandle handle = calloc(1, sizeof(struct sIedDiscoveryHandle));
    if (!handle) {
        if (outError) *outError = IED_DISCOVERY_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config) {
        handle->config = *config;
    } else {
        IedDiscoveryConfig_defaults(&handle->config);
    }

    handle->ownedAuthPassword = IedDiscoveryUtils_safeStringDup(handle->config.acseAuthPassword);
    handle->config.acseAuthPassword = handle->ownedAuthPassword;

    /* Delegate purely for SclBootstrap_tcpProbeOnly - see
     * sIedDiscoveryHandle's own doc comment for why this is reused rather
     * than duplicated. Only the two probe-relevant fields are copied over;
     * the delegate never calls SclBootstrap_scanAndFetch, so its other
     * config fields (mmsConnectTimeoutMs, maxBrowseDepth, acseAuthPassword)
     * are irrelevant and left at their own defaults. */
    SclBootstrapConfig delegateConfig;
    SclBootstrapConfig_defaults(&delegateConfig);
    delegateConfig.tcpProbeTimeoutMs = handle->config.tcpProbeTimeoutMs;
    delegateConfig.maxConcurrentTcpProbes = handle->config.maxConcurrentTcpProbes;

    SclBootstrapError delegateErr;
    handle->tcpProbeDelegate = SclBootstrap_create(&delegateConfig, &delegateErr);
    if (!handle->tcpProbeDelegate) {
        free(handle->ownedAuthPassword);
        free(handle);
        if (outError) *outError = IED_DISCOVERY_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (outError) *outError = IED_DISCOVERY_OK;
    return handle;
}

static IedDiscoveryHostStatus
verifyOneHost(IedDiscoveryHandle handle, const char* host, int mmsPort) {
    LinkedList single = LinkedList_create();
    LinkedList_add(single, (void*) host);

    bool* reachable = SclBootstrap_tcpProbeOnly(handle->tcpProbeDelegate, single, mmsPort);
    LinkedList_destroyStatic(single); /* host is caller-owned, not this function's to free */

    if (!reachable) return IED_DISCOVERY_HOST_NOT_TCP_REACHABLE;
    bool tcpOk = reachable[0];
    free(reachable);

    if (!tcpOk) return IED_DISCOVERY_HOST_NOT_TCP_REACHABLE;

    bool associated = IedDiscoveryMmsProbe_associate(host, mmsPort, handle->config.mmsConnectTimeoutMs,
            handle->ownedAuthPassword);
    return associated ? IED_DISCOVERY_HOST_CONFIRMED : IED_DISCOVERY_HOST_NOT_MMS_DEVICE;
}

IedDiscoveryHostStatus
IedDiscovery_verifyHost(IedDiscoveryHandle handle, const char* host, int mmsPort, IedDiscoveryError* outError) {
    if (!handle || !host || !host[0] || mmsPort <= 0) {
        if (outError) *outError = IED_DISCOVERY_ERR_INVALID_ARGUMENT;
        return IED_DISCOVERY_HOST_NOT_TCP_REACHABLE;
    }

    if (outError) *outError = IED_DISCOVERY_OK;
    return verifyOneHost(handle, host, mmsPort);
}

LinkedList
IedDiscovery_scanSubnet(IedDiscoveryHandle handle, const char* interfaceId, int mmsPort,
        IedDiscoveryError* outError) {
    if (!handle || !interfaceId || !interfaceId[0] || mmsPort <= 0) {
        if (outError) *outError = IED_DISCOVERY_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    uint32_t address;
    uint32_t netmask;
    if (!IedDiscoveryNetif_getInterfaceIpv4(interfaceId, &address, &netmask)) {
        if (outError) *outError = IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND;
        return NULL;
    }

    if (IedDiscoveryCidr_hostCount(netmask) > handle->config.maxHosts) {
        if (outError) *outError = IED_DISCOVERY_ERR_SUBNET_TOO_LARGE;
        return NULL;
    }

    LinkedList candidates = IedDiscoveryCidr_buildCandidateList(address, netmask, address, handle->config.maxHosts);
    if (!candidates) {
        if (outError) *outError = IED_DISCOVERY_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    LinkedList confirmed = LinkedList_create();
    if (!confirmed) {
        LinkedList_destroyDeep(candidates, free);
        if (outError) *outError = IED_DISCOVERY_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (LinkedList_size(candidates) == 0) {
        LinkedList_destroyStatic(candidates);
        if (outError) *outError = IED_DISCOVERY_OK;
        return confirmed;
    }

    bool* reachable = SclBootstrap_tcpProbeOnly(handle->tcpProbeDelegate, candidates, mmsPort);
    if (!reachable) {
        LinkedList_destroyDeep(candidates, free);
        LinkedList_destroyStatic(confirmed);
        if (outError) *outError = IED_DISCOVERY_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    int index = 0;
    LinkedList element = LinkedList_getNext(candidates);
    while (element) {
        const char* host = (const char*) LinkedList_getData(element);

        if (reachable[index]
                && IedDiscoveryMmsProbe_associate(host, mmsPort, handle->config.mmsConnectTimeoutMs,
                        handle->ownedAuthPassword)) {
            LinkedList_add(confirmed, IedDiscoveryUtils_safeStringDup(host));
        }

        element = LinkedList_getNext(element);
        index++;
    }

    free(reachable);
    LinkedList_destroyDeep(candidates, free);

    if (outError) *outError = IED_DISCOVERY_OK;
    return confirmed;
}

void
IedDiscovery_destroy(IedDiscoveryHandle handle) {
    if (!handle) return;

    SclBootstrap_destroy(handle->tcpProbeDelegate);
    free(handle->ownedAuthPassword);
    free(handle);
}
