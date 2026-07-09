#ifndef SCL_BOOTSTRAP_API_H_
#define SCL_BOOTSTRAP_API_H_

#include "linked_list.h"
#include "features/scl_bootstrap/domain/scl_bootstrap_types.h"

/*
 * Public boundary of the scl_bootstrap feature. Other features/callers
 * (main.c, a future ipc_dispatcher) should only ever include this header -
 * never reach into domain/data/utils directly.
 *
 * ONLY job: given a caller-supplied list of candidate addresses, find which
 * ones speak MMS on the given port, and for each one found, browse its file
 * directory and try to fetch one SCL file (.icd/.cid/.scd/.ssd/.sed),
 * returning the raw bytes + filename. Does NOT load anything into ied_model
 * and does NOT enable GOOSE/MMS reporting - purely a bootstrap/probe utility.
 *
 * One-shot, blocking, synchronous: no background thread, no reconnect
 * supervisor, no callback-delivered result stream - scanning a network is
 * fundamentally a "do it once, get a complete answer" operation, unlike
 * mms_report_client/goose_subscriber's long-running background workers.
 * There is deliberately no _start()/_stop() pair: SclBootstrap_scanAndFetch
 * IS the one blocking entry point, and there is never a background operation
 * in flight for _destroy() to have to tear down.
 */

void
SclBootstrapConfig_defaults(SclBootstrapConfig* config);

/* Allocates only - no I/O. config: NULL means SclBootstrapConfig_defaults(). */
SclBootstrapHandle
SclBootstrap_create(const SclBootstrapConfig* config, SclBootstrapError* outError);

/*
 * Optional, may be left unset/NULL. If set, fires synchronously on the
 * calling thread once per candidate, immediately after that candidate's
 * result is finalized - there is no background thread here, unlike
 * mms_report_client/goose_subscriber's callbacks, so this is a plain,
 * reentrant function call, not a cross-thread delivery. Purely for
 * incremental progress reporting on a large host list; scanAndFetch's return
 * value is always the complete, authoritative result set regardless of
 * whether this is registered.
 */
void
SclBootstrap_setProgressCallback(SclBootstrapHandle handle,
        SclBootstrapProgressCallback callback, void* userParam);

/*
 * Blocking. hostList: LinkedList of char* candidate hostnames/IP literals -
 * borrowed, not retained past this call, copied internally as needed.
 * mmsPort: usually 102, caller-supplied so non-standard ports/simulators work.
 *
 * Scans every candidate (bounded-parallel TCP probe), then for each candidate
 * that answered on the port, attempts a full MMS association and SCL file
 * fetch (sequential).
 *
 * Returns a LinkedList of SclBootstrapResult*, one per input candidate, in
 * input order - callers get a complete picture, including the ones that
 * didn't pan out. Caller owns the list and its elements:
 * LinkedList_destroyDeep(list, SclBootstrap_destroyResult).
 *
 * Returns NULL and sets *outError only for argument-level failures (NULL/
 * empty hostList, bad port, allocation failure) - per-candidate connect/fetch
 * outcomes are reported via each result's .status field, never via outError.
 */
LinkedList
SclBootstrap_scanAndFetch(SclBootstrapHandle handle, LinkedList hostList, int mmsPort,
        SclBootstrapError* outError);

/* LinkedListValueDeleteFunction-compatible: frees one SclBootstrapResult,
 * including its owned host/fileName/fileData buffers. NULL-safe. */
void
SclBootstrap_destroyResult(void* result);

/* Frees the handle. No stop() exists: scanAndFetch is synchronous, so there
 * is never a background operation in flight to tear down - destroy is only
 * ever called after scanAndFetch has already returned. */
void
SclBootstrap_destroy(SclBootstrapHandle handle);

/*
 * Phase-1-only TCP reachability probe, exposed for sibling features (e.g.
 * ied_discovery) that need the same bounded-concurrency async TCP scan
 * without wanting scl_bootstrap's full MMS-association + SCL browse/fetch
 * too. Thin wrapper around the exact SclBootstrapTcpProbe_scan machinery
 * SclBootstrap_scanAndFetch already uses for its own phase 1 - reused rather
 * than duplicated because that async-connect sliding-window state machine is
 * substantial and easy to get subtly wrong a second time, unlike the small
 * ACSE-auth-setup snippet mms_report_client duplicates from this feature
 * (see mms_report_client's own Architecture bullet in CLAUDE.md).
 *
 * Reuses handle->config.tcpProbeTimeoutMs/maxConcurrentTcpProbes - no
 * separate config parameter.
 *
 * Returns a newly malloc'd bool array, LinkedList_size(hostList) entries, in
 * the same order/ownership contract as SclBootstrapTcpProbe_scan itself
 * (caller must free()). NULL on NULL handle/hostList, empty hostList, bad
 * port, or allocation failure.
 */
bool*
SclBootstrap_tcpProbeOnly(SclBootstrapHandle handle, LinkedList hostList, int mmsPort);

#endif /* SCL_BOOTSTRAP_API_H_ */
