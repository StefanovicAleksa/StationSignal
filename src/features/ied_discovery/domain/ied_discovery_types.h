#ifndef IED_DISCOVERY_TYPES_H_
#define IED_DISCOVERY_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "features/scl_bootstrap/service/scl_bootstrap_api.h"

/*
 * Domain vocabulary for this feature is deliberately small: it never touches
 * SCL or the data model at all (see this feature's own Architecture bullet
 * in CLAUDE.md) - its only job is "which hosts on this subnet are really
 * speaking IEC 61850 MMS", handing off a plain host string to the existing,
 * unmodified scl_bootstrap/orchestration pipeline once one is picked.
 */

typedef enum {
    IED_DISCOVERY_OK = 0,
    IED_DISCOVERY_ERR_INVALID_ARGUMENT,
    IED_DISCOVERY_ERR_OUT_OF_MEMORY,
    IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND, /* named interface missing, down, or has no IPv4 address */
    IED_DISCOVERY_ERR_SUBNET_TOO_LARGE     /* host count > config.maxHosts - safety valve */
} IedDiscoveryError;

typedef enum {
    /* Both the TCP probe and the real MMS/ACSE association succeeded. */
    IED_DISCOVERY_HOST_CONFIRMED = 0,

    /* Phase 1 (TCP connect) never succeeded within tcpProbeTimeoutMs. */
    IED_DISCOVERY_HOST_NOT_TCP_REACHABLE,

    /* TCP was reachable, but the real MMS/ACSE association (phase 2) failed
     * or was rejected (including after an auth retry, if configured) - so
     * whatever answered isn't a real IEC 61850 MMS device, or requires
     * credentials this handle doesn't have. */
    IED_DISCOVERY_HOST_NOT_MMS_DEVICE
} IedDiscoveryHostStatus;

typedef struct {
    uint32_t tcpProbeTimeoutMs;      /* default 500 - phase 1 per-candidate bound */
    int      maxConcurrentTcpProbes; /* default 64 - phase 1 sliding-window width;
                                         higher than scl_bootstrap's own 16-host default
                                         since a subnet scan can mean up to a few hundred
                                         candidates, not a handful of manually-typed hosts */
    uint32_t mmsConnectTimeoutMs;    /* default 3000 - phase 2 association bound */
    uint32_t maxHosts;               /* default 1024 - ceiling on a scanned subnet's host
                                         count; scanSubnet fails fast with
                                         IED_DISCOVERY_ERR_SUBNET_TOO_LARGE rather than
                                         silently probing a misconfigured huge range */
    const char* acseAuthPassword;    /* NULL = never retry with auth; copied internally;
                                         independent of scl_bootstrap's/mms_report_client's
                                         own - features never share config structs across
                                         the public boundary */
} IedDiscoveryConfig;

/*
 * Internal representation, defined here rather than hidden - opacity is
 * enforced by which header is exposed (service/ied_discovery_api.h is the
 * only public one), matching scl_bootstrap/ied_model's own convention.
 */
struct sIedDiscoveryHandle {
    IedDiscoveryConfig config;
    char* ownedAuthPassword; /* owned copy of config.acseAuthPassword, or NULL */

    /* Owned purely to call SclBootstrap_tcpProbeOnly - a legitimate
     * cross-feature call via scl_bootstrap's public service API, reusing its
     * nontrivial async TCP-probe state machine rather than duplicating it
     * (see that function's own doc comment for the reasoning). */
    SclBootstrapHandle tcpProbeDelegate;
};
typedef struct sIedDiscoveryHandle* IedDiscoveryHandle;

#endif /* IED_DISCOVERY_TYPES_H_ */
