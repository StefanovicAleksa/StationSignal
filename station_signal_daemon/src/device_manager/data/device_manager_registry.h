#ifndef DEVICE_MANAGER_REGISTRY_H_
#define DEVICE_MANAGER_REGISTRY_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "device_manager/domain/device_manager_types.h"

/*
 * Mutex-guarded bookkeeping of every currently-starting-or-running device:
 * deviceId generation, the entries array, and the per-device websocket-port
 * allocator (device_manager_port_allocator.h). Opaque outside this file.
 *
 * TWO-PHASE LOCKING IS THE KEY DESIGN POINT, same reasoning as
 * scan_orchestration_registry.c's own top comment: Orchestration_run (any of
 * its three variants) and Orchestration_stop can each block for seconds (real
 * network I/O - SCL bootstrap, MMS connect/disconnect), so this registry must
 * NEVER hold its lock across either call - otherwise one device's slow
 * start/stop would serialize every other concurrent device's start/stop
 * behind it.
 *
 * Unlike scan_orchestration's registry, START needs a THIRD phase: the
 * deviceId + websocket port must be reserved (and a placeholder entry made
 * visible to the host-dedupe check) BEFORE the slow Orchestration_run* call,
 * so two concurrent starts never race for the same port or duplicate a host,
 * and the reservation must be finalized (or rolled back) in a SEPARATE,
 * later, short critical section once that slow call returns:
 *
 *   reserve (short lock) -> Orchestration_create/_run* (NO lock) ->
 *     finalize or rollback (short lock)
 *
 * Stop only needs the two phases scan_orchestration's own registry already
 * has: removeIfRunning (short lock, atomic find-and-remove so a concurrent
 * duplicate stop can never double-tear-down) -> Orchestration_stop/_destroy
 * (NO lock) -> freePort (short lock).
 *
 * acseAuthPassword MUST be an OWNED copy, never the caller's borrowed
 * pointer: OrchestrationConfig documents its own acseAuthPassword fields as
 * "stays borrowed... caller must keep it alive for the OrchestrationHandle's
 * whole lifetime" - true for main.c's own argv (alive for the whole
 * process), but NOT true once a value arrives via a parsed control-plane
 * message, whose source buffer is freed the moment that one request finishes
 * processing. DeviceManagerRegistry_reserve therefore strdups it immediately
 * and hands back a BORROWED pointer to its own copy (via
 * outOwnedAcseAuthPassword) for the caller to wire straight into
 * OrchestrationConfig - the registry keeps ownership; the copy is freed only
 * after Orchestration_destroy actually runs (rollback or stop), never before.
 */
typedef struct sDeviceManagerRegistry* DeviceManagerRegistry;

DeviceManagerRegistry
DeviceManagerRegistry_create(uint16_t wsPortRangeStart, uint16_t wsPortRangeEnd);

/* Does NOT stop/destroy any OrchestrationHandle still registered - caller
 * (DeviceManager_destroy) must drain every running device via the public
 * stopReporting path first. Only frees this registry's own bookkeeping
 * (array, port allocator, owned host/password copies, lock). NULL-safe. */
void
DeviceManagerRegistry_destroy(DeviceManagerRegistry registry);

/*
 * PHASE A (start). Rejects with DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING if
 * this exact (host, mmsPort) pair already has an entry (running or still
 * mid-start) - a linear scan over entries, expected to stay small (tens, not
 * thousands, of concurrently reporting devices). Otherwise: strdups host and
 * acseAuthPassword (acseAuthPassword may be NULL - unauthenticated),
 * allocates a deviceId (monotonic, starts at 1) and a websocket port
 * together, and inserts a running=false placeholder entry.
 * *outOwnedAcseAuthPassword is set to a BORROWED pointer to the registry's
 * own copy (NULL if acseAuthPassword was NULL) - valid until this device is
 * rolled back or stopped, for the caller to wire directly into
 * OrchestrationConfig without a second strdup. On
 * DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING/_PORT_EXHAUSTED/_OUT_OF_MEMORY,
 * nothing is inserted and every out-param is left untouched.
 */
DeviceManagerError
DeviceManagerRegistry_reserve(DeviceManagerRegistry registry, const char* host, int mmsPort,
        const char* acseAuthPassword, uint64_t* outDeviceId, uint16_t* outWsPort,
        const char** outOwnedAcseAuthPassword);

/* PHASE C-success (start). Finds deviceId's placeholder and attaches the
 * now-successfully-created handle, flips running=true. No-op if deviceId
 * isn't found (should not happen in practice - only this handle's own
 * reserve call ever creates it). */
void
DeviceManagerRegistry_finalize(DeviceManagerRegistry registry, uint64_t deviceId, OrchestrationHandle handle);

/* PHASE C-failure (start). Removes deviceId's placeholder entirely, frees its
 * port back to the allocator, frees the owned host/password copies. Caller
 * must have already called Orchestration_destroy on the failed handle OUTSIDE
 * any lock before calling this - this function never touches the handle. */
void
DeviceManagerRegistry_rollback(DeviceManagerRegistry registry, uint64_t deviceId);

/*
 * PHASE A (stop). Atomic find-and-remove: DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND
 * if no entry has this id; DEVICE_MANAGER_ERR_START_IN_PROGRESS if the entry
 * exists but is still mid-start (running==false) on another thread - no
 * cancellation hook exists, same accepted limitation
 * ScanOrchestrationWorker_stop already documents for its own in-flight sweep.
 * Otherwise removes the entry right here (a concurrent duplicate stop for the
 * same deviceId can never find it again) and hands back the owned handle/
 * port/host/password (as owned pointers - caller now owns host/password and
 * must free them, only AFTER Orchestration_destroy has actually run) for the
 * caller to tear down OUTSIDE the lock. outOwnedHost/outOwnedAcseAuthPassword
 * are NULL-safe - passing NULL frees that copy immediately instead of
 * handing it back.
 */
DeviceManagerError
DeviceManagerRegistry_removeIfRunning(DeviceManagerRegistry registry, uint64_t deviceId,
        OrchestrationHandle* outHandle, uint16_t* outPort, char** outOwnedHost, char** outOwnedAcseAuthPassword);

/* PHASE C (stop). Returns port to the allocator's free-list. */
void
DeviceManagerRegistry_freePort(DeviceManagerRegistry registry, uint16_t port);

/*
 * Resolves (host, mmsPort) to the deviceId of whatever entry currently
 * occupies it - running OR still mid-start, same match scope as the
 * hostAlreadyActive dedupe check reserve() uses internally. Returns true and
 * sets *outDeviceId if found, false (outDeviceId untouched) otherwise. Used
 * by DeviceManager_stopReportingByAddress to recover a deviceId a caller
 * never obtained/kept (e.g. its own StartReporting call raced or timed out
 * client-side, or a prior process instance's bookkeeping was lost) - once
 * resolved, the normal deviceId-keyed stop path (removeIfRunning) applies
 * unchanged, including its own START_IN_PROGRESS protection for a mid-start
 * entry.
 */
bool
DeviceManagerRegistry_findDeviceIdByHost(DeviceManagerRegistry registry, const char* host, int mmsPort,
        uint64_t* outDeviceId);

/* Any one currently RUNNING (not mid-start) deviceId - used only by
 * DeviceManager_destroy to drain the registry one device at a time via the
 * public stopReporting path. 0 if none (0 is never a valid deviceId). A
 * device still mid-start at destroy time (an extremely narrow race - a
 * concurrent StartReporting call landing exactly during process shutdown) is
 * not returned here and is an accepted, undrained limitation of this v1. */
uint64_t
DeviceManagerRegistry_anyRunningDeviceId(DeviceManagerRegistry registry);

#endif /* DEVICE_MANAGER_REGISTRY_H_ */
