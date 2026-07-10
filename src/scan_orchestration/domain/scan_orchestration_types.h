#ifndef SCAN_ORCHESTRATION_TYPES_H_
#define SCAN_ORCHESTRATION_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "features/ied_discovery/service/ied_discovery_api.h"
#include "features/scan_dispatcher/service/scan_dispatcher_api.h"

/*
 * Domain vocabulary for this layer is entirely our own sibling features'
 * public APIs (ied_discovery, scan_dispatcher) - never their domain/data
 * headers directly, same rule src/orchestration/domain/orchestration_types.h
 * states about itself. Zero direct third-party includes of its own -
 * this layer's job is sequencing already-implemented features (a continuous
 * background wrapper around ied_discovery's synchronous scanSubnet, streamed
 * out over scan_dispatcher's websocket), not talking to libiec61850/
 * libwebsockets/cJSON itself.
 */

typedef enum {
    SCAN_ORCHESTRATION_OK = 0,
    SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT,
    SCAN_ORCHESTRATION_ERR_OUT_OF_MEMORY,
    SCAN_ORCHESTRATION_ERR_DISPATCHER_START_FAILED, /* 0->1 active-scan transition's ScanDispatcher_start failed */
    SCAN_ORCHESTRATION_ERR_THREAD_CREATE_FAILED,
    SCAN_ORCHESTRATION_ERR_DISCOVERY_CREATE_FAILED, /* per-scan IedDiscovery_create failed */
    SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND            /* scanId isn't currently active */
} ScanOrchestrationError;

typedef struct {
    const char* interfaceId;       /* borrowed at call time, deep-copied internally by the worker */
    int mmsPort;
    uint32_t sweepIntervalMs;      /* 0 => config.defaultSweepIntervalMs */
    const char* acseAuthPassword;  /* borrowed, deep-copied internally (via IedDiscovery_create's own
                                       dup); NULL = unauthenticated. Per-request, not per-handle, since
                                       concurrent scans may target devices needing different credentials */
} ScanRequest;

typedef struct {
    ScanDispatcherConfig scanDispatcherConfig;   /* port defaults 8766 */
    uint32_t defaultSweepIntervalMs;             /* default 10000 (10s) */
    IedDiscoveryConfig discoveryConfigTemplate;  /* tcpProbeTimeoutMs/maxConcurrentTcpProbes/
                                                     mmsConnectTimeoutMs/maxHosts reused per-scan
                                                     verbatim; its own .acseAuthPassword field is
                                                     IGNORED - ScanRequest.acseAuthPassword wins per
                                                     scan instead, since different concurrent scans
                                                     may need different credentials */
} ScanOrchestrationConfig;

/*
 * Fires once per genuinely NEW host, on the scan's own worker thread - MUST
 * NOT block (same contract as MmsReportClientConnStateCallback/
 * GooseSubscriberStatusCallback). One process-wide slot per
 * ScanOrchestrationHandle (not per-scan, mirrors Orchestration's own single-
 * callback-slot convention) - set before any ScanOrchestration_startScan
 * call; a worker snapshots the pointer at creation time, so a callback
 * change after a scan has already started does not retroactively apply to it.
 */
typedef void (*ScanOrchestrationDeviceFoundCallback)(void* userParam, uint64_t scanId,
        const char* host, int mmsPort);

struct sScanOrchestrationRegistry; /* opaque, defined in data/scan_orchestration_registry.c */

struct sScanOrchestrationHandle {
    ScanOrchestrationConfig config;
    ScanDispatcherHandle scanDispatcher;          /* owned, created (alloc-only) at _create */
    struct sScanOrchestrationRegistry* registry;  /* owned */
    ScanOrchestrationDeviceFoundCallback foundCallback;
    void* foundCallbackParam;
};
typedef struct sScanOrchestrationHandle* ScanOrchestrationHandle;

#endif /* SCAN_ORCHESTRATION_TYPES_H_ */
