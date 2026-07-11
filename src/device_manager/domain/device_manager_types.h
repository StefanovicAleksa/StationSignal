#ifndef DEVICE_MANAGER_TYPES_H_
#define DEVICE_MANAGER_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "orchestration/service/orchestration_api.h"

/*
 * Domain vocabulary for this layer is entirely orchestration's own public API
 * (OrchestrationHandle/OrchestrationError/OrchestrationErrorDetail, plus
 * AccessMode transitively via ied_model's public header) - never
 * orchestration's domain/data headers directly, same rule orchestration/
 * itself states about its own relationship to scl_bootstrap/ied_model/etc.
 * Zero direct third-party includes of its own - this layer's job is running
 * SEVERAL orchestration pipelines concurrently (one per physical IED) and
 * allocating each its own ipc_dispatcher websocket port, not talking to
 * libiec61850/libwebsockets/cJSON itself.
 */

typedef enum {
    DEVICE_MANAGER_OK = 0,
    DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
    DEVICE_MANAGER_ERR_OUT_OF_MEMORY,
    DEVICE_MANAGER_ERR_PORT_EXHAUSTED,        /* configured wsPortRange is fully allocated */
    DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING,  /* this (host, mmsPort) already has a running or
                                                  mid-start device - default dedupe policy, see
                                                  service/device_manager_api.h */
    DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED,  /* see DeviceManagerErrorDetail */
    DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND,      /* stopReporting: unknown/already-stopped deviceId */
    DEVICE_MANAGER_ERR_START_IN_PROGRESS      /* stopReporting: deviceId exists but its
                                                  startReporting call hasn't finished the slow
                                                  phase yet on another thread - no cancellation
                                                  hook exists, retry after a delay */
} DeviceManagerError;

/* Richer diagnostics for DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED, passed
 * through verbatim from whichever Orchestration_create/_run* call failed -
 * mirrors this codebase's existing "status+detail two-part" convention
 * (SclBootstrapResult, OrchestrationErrorDetail itself). Zero-valued/ignored
 * for every other DeviceManagerError. */
typedef struct {
    OrchestrationError orchestrationError;
    OrchestrationErrorDetail orchestrationDetail;
} DeviceManagerErrorDetail;

typedef struct {
    uint16_t wsPortRangeStart;  /* default 9000 */
    uint16_t wsPortRangeEnd;    /* default 9999 - inclusive; DEVICE_MANAGER_ERR_PORT_EXHAUSTED
                                   once every port in [start,end] is allocated simultaneously */
} DeviceManagerConfig;

struct sDeviceManagerRegistry; /* opaque, defined in data/device_manager_registry.c */

struct sDeviceManagerHandle {
    DeviceManagerConfig config;
    struct sDeviceManagerRegistry* registry; /* owned */
};
typedef struct sDeviceManagerHandle* DeviceManagerHandle;

#endif /* DEVICE_MANAGER_TYPES_H_ */
