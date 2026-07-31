#ifndef DEVICE_MANAGER_API_H_
#define DEVICE_MANAGER_API_H_

#include <stdint.h>
#include "device_manager/domain/device_manager_types.h"

/*
 * Public boundary of device_manager. Other code (main.c, control_dispatcher)
 * should only ever include this header - never reach into domain/data
 * directly.
 *
 * A top-level sibling of src/features/, src/orchestration/, and
 * src/scan_orchestration/ (not itself a "feature" in CLAUDE.md's
 * Expected-features sense) - runs SEVERAL orchestration pipelines
 * concurrently, one per physical IED, each with its own auto-assigned
 * ipc_dispatcher websocket port, addressable by a server-generated deviceId.
 * A synchronous library, no thread of its own (same as
 * ScanOrchestration_startScan/_stopScan) - StartReporting/StopReporting each
 * block the calling thread for as long as the underlying Orchestration_run*
 * or Orchestration_stop call takes, but never serialize behind a DIFFERENT
 * device's own slow call (two-phase-locked registry, see
 * data/device_manager_registry.h).
 */

/* Fills config with recommended defaults: wsPortRangeStart=9000,
 * wsPortRangeEnd=9999. Caller may override before DeviceManager_create. */
void
DeviceManagerConfig_defaults(DeviceManagerConfig* config);

/* Allocates only - creates the registry (its own port allocator + mutex),
 * no I/O. config: NULL means DeviceManagerConfig_defaults(). Returns NULL
 * and sets *outError on argument (wsPortRangeStart > wsPortRangeEnd) or
 * allocation failure only. */
DeviceManagerHandle
DeviceManager_create(const DeviceManagerConfig* config, DeviceManagerError* outError);

/*
 * Blocking. Validates arguments first (host non-empty, interfaceId
 * non-empty, and - per this feature's own explicit requirement - iedName
 * non-empty whenever sclFilePath is given) before touching the registry at
 * all, returning DEVICE_MANAGER_ERR_INVALID_ARGUMENT immediately on failure.
 * Then:
 *   1. Reserves a deviceId + auto-assigned websocket port together (short
 *      lock) - DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING if this exact
 *      (host, mmsPort) is already running or mid-start (a default dedupe
 *      policy - not dictated by the original ask, trivially relaxed by
 *      deleting the check in device_manager_registry.c if a caller ever
 *      legitimately wants two independent handles against one device);
 *      DEVICE_MANAGER_ERR_PORT_EXHAUSTED if the configured port range is
 *      fully allocated.
 *   2. Builds an OrchestrationConfig (defaults, ipcDispatcherConfig.port =
 *      the reserved port, bootstrapConfig/reportClientConfig.acseAuthPassword
 *      = the registry's own owned copy of acseAuthPassword - see
 *      device_manager_registry.h's own top comment for why this must be an
 *      owned copy, never the caller's borrowed pointer), creates an
 *      OrchestrationHandle, and runs DeviceManagerBootstrapPolicy_run against
 *      it (NO registry lock held - this is the slow part: real network I/O,
 *      seconds-scale).
 *   3. On success: attaches the handle to the reservation (short lock),
 *      returns DEVICE_MANAGER_OK with outDeviceId/outWsPort set. On
 *      failure: destroys the just-created OrchestrationHandle (outside any
 *      lock), then rolls back the reservation (short lock, frees the port and
 *      the owned host/password copies), returns
 *      DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED with
 *      outDetail->orchestrationError/orchestrationDetail set to whatever the
 *      failing Orchestration_create/DeviceManagerBootstrapPolicy_run call
 *      returned.
 *
 * sclFilePath/acseAuthPassword/iedName: NULL or "" means "not given"
 * (optional). iedName empty is allowed only when sclFilePath is NOT given -
 * Orchestration_run's own existing auto-detect semantics (exactly one <IED>
 * in the fetched SCL); required and validated up front when sclFilePath IS
 * given, since Orchestration_runFromLocalFile's own auto-detect would still
 * technically work, but this feature's own explicit contract makes it
 * mandatory instead.
 *
 * outMmsAvailable/outGooseAvailable are optional (NULL-safe) and, on
 * DEVICE_MANAGER_OK, report which of MMS reporting / GOOSE subscription this
 * device's SCL actually declared targets for - see
 * Orchestration_run's own doc comment for the full contract. A device
 * declaring only one of the two now still succeeds; only a device declaring
 * neither fails (DEVICE_MANAGER_ERR_ORCHESTRATION_FAILED with
 * outDetail->orchestrationError == ORCHESTRATION_ERR_NO_CAPABILITIES).
 */
DeviceManagerError
DeviceManager_startReporting(DeviceManagerHandle handle, const char* host, int mmsPort,
        const char* iedName, const char* interfaceId, const char* sclFilePath,
        const char* acseAuthPassword, AccessMode accessMode,
        uint64_t* outDeviceId, uint16_t* outWsPort, DeviceManagerErrorDetail* outDetail,
        bool* outMmsAvailable, bool* outGooseAvailable);

/*
 * Blocking. Looks up deviceId (short lock, atomic remove so a concurrent
 * duplicate stop for the same id always fails cleanly with
 * DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND rather than double-tearing-down):
 *   - DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND if unknown/already stopped.
 *   - DEVICE_MANAGER_ERR_START_IN_PROGRESS if deviceId exists but is still
 *     mid-start on another thread - no cancellation hook exists; retry after
 *     a delay if "stop as soon as possible" is needed.
 * Otherwise: Orchestration_stop + Orchestration_destroy (NO lock held - same
 * reverse-order teardown already implemented in orchestration_api.c: goose ->
 * report client -> ipc_dispatcher -> ied_model), then frees this device's
 * owned host/password copies and returns the port to the allocator (short
 * lock). outDetail is optional (NULL-safe) - only the DeviceManagerError
 * codes above are ever possible here, no OrchestrationErrorDetail to report
 * (orchestrationDetail is left zero-valued).
 */
DeviceManagerError
DeviceManager_stopReporting(DeviceManagerHandle handle, uint64_t deviceId, DeviceManagerErrorDetail* outDetail);

/*
 * Blocking. Recovery path for a caller that never obtained or has lost track
 * of a deviceId (e.g. its own prior StartReporting call raced/timed out
 * client-side, or an owning process instance restarted and its own
 * bookkeeping was lost) - resolves (host, mmsPort) to whatever deviceId
 * currently occupies it (DeviceManagerRegistry_findDeviceIdByHost), then
 * delegates to DeviceManager_stopReporting unchanged, so every existing
 * behavior/error code (DEVICE_MANAGER_ERR_START_IN_PROGRESS for a mid-start
 * entry, atomic remove against a concurrent duplicate stop, teardown
 * ordering) applies identically - this is purely an alternate key, not a
 * different stop path. DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND if nothing is
 * registered at this address (a legitimate, idempotent "already clean"
 * outcome for a caller recovering from an unknown prior state, not
 * necessarily a real failure). On DEVICE_MANAGER_OK, *outDeviceId is set to
 * whichever device was actually stopped, since the caller had no way to know
 * it in advance.
 */
DeviceManagerError
DeviceManager_stopReportingByAddress(DeviceManagerHandle handle, const char* host, int mmsPort,
        uint64_t* outDeviceId, DeviceManagerErrorDetail* outDetail);

/* Stops+destroys every still-running device (drains via the public
 * stopReporting path, one at a time - see DeviceManagerRegistry_anyRunningDeviceId's
 * own doc comment for the narrow mid-start-at-shutdown limitation), then
 * frees the registry and the handle. NULL-safe. */
void
DeviceManager_destroy(DeviceManagerHandle handle);

#endif /* DEVICE_MANAGER_API_H_ */
