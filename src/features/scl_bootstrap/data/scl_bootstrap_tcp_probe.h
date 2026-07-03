#ifndef SCL_BOOTSTRAP_TCP_PROBE_H_
#define SCL_BOOTSTRAP_TCP_PROBE_H_

#include <stdint.h>
#include "linked_list.h"
#include "stdbool_compat.h"

/*
 * Phase 1 of scanning: a cheap, bounded-timeout TCP liveness probe across a
 * candidate host list, built directly on hal_socket.h's async-connect
 * primitives (Socket_connectAsync/Socket_checkAsyncConnectState) - a sliding
 * window of up to maxConcurrent probes in flight at once, all driven from one
 * control thread (no OS thread pool needed: each probe is just a bounded
 * connect wait, not sustained I/O).
 *
 * A successful TCP connect here does NOT mean "this is an MMS device" - it
 * only proves something is listening on the port. That stronger claim is
 * reserved for phase 2's actual IedConnection_connect (see
 * scl_bootstrap_mms_session.h), which performs the real COTP/session/
 * presentation/ACSE/MMS handshake.
 */

/*
 * Returns a newly malloc'd bool array with LinkedList_size(hostList) entries,
 * in the same order as hostList's char* elements: true if a TCP connection to
 * (that host, port) was established within timeoutMs, false otherwise
 * (refused, unreachable, or timed out). Caller must free() the returned
 * array. Returns NULL if hostList is NULL/empty or on allocation failure.
 */
bool*
SclBootstrapTcpProbe_scan(LinkedList hostList, int port, uint32_t timeoutMs, int maxConcurrent);

#endif /* SCL_BOOTSTRAP_TCP_PROBE_H_ */
