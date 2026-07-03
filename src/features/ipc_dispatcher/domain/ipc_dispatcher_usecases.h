#ifndef IPC_DISPATCHER_USECASES_H_
#define IPC_DISPATCHER_USECASES_H_

#include <stddef.h>
#include "stdbool_compat.h"
#include "features/ipc_dispatcher/domain/ipc_dispatcher_types.h"

/*
 * Pure logic - zero third-party includes, unit-testable with hand-built
 * plain arrays only (no MmsValue/cJSON/lws anywhere near this file).
 *
 * Reference format (confirmed via ied_model_usecases.c's
 * IedModelUseCases_getDataSetMemberReferences): "<LDName>/<LN>$<FC>$<DO>$<DA>"
 * - e.g. "LD0/LLN0$ST$Ind1$stVal" and "LD0/LLN0$ST$Ind1$q" share everything up
 * to the LAST '$'.
 */

/*
 * Splits `reference` on its LAST '$' into a prefix (length outPrefixLen, NOT
 * including the '$') and a daName (outDaName points just past the last '$',
 * into the original string - no allocation). Returns false (outPrefixLen/
 * outDaName untouched) if reference is NULL or has no '$' at all.
 */
bool
IpcDispatcherUseCases_splitReference(const char* reference, size_t* outPrefixLen, const char** outDaName);

/*
 * Pairs `count` raw per-entry reference strings (references[i] may be NULL;
 * this INCLUDES "q" entries at their own original index) by shared prefix:
 *   - An entry whose daName == "q" is never itself emitted as a value entry
 *     (whether or not it has a same-prefix sibling - a lone "q" is simply
 *     dropped, not fabricated into a value-less data point).
 *   - Every other entry (non-"q" daName, or an unparseable/NULL reference) is
 *     emitted as a value entry, in original index order, paired with the
 *     index of a same-prefix "q" sibling if one exists (-1 otherwise).
 *
 * Writes into caller-supplied arrays (room for `count` elements each):
 *   outValueIndices[k]         = original index of the k-th value entry
 *   outQualityIndexForValue[k] = original index of that value's "q" sibling,
 *                                 or -1 if none
 * Returns the number of value entries written (0..count).
 *
 * O(count^2) (pairwise prefix comparison) - fine for realistic dataset sizes
 * (tens of entries, not thousands).
 */
int
IpcDispatcherUseCases_pairQuality(const char* const* references, int count,
        int* outValueIndices, int* outQualityIndexForValue);

/*
 * Deep-copies already-paired, already-converted parallel arrays (each of
 * length pointCount) into one owned IpcMessage - does no further pairing;
 * the caller runs IpcDispatcherUseCases_pairQuality itself first, converts
 * the relevant MmsValues via utils/ipc_dispatcher_value_codec, then calls
 * this (see data/ adapters). Every string is deep-copied (pointReferences[k],
 * and pointValues[k].value.str for STRING/RAW) - caller's own arrays/strings
 * may be freed immediately after this returns. sourceReference may be NULL
 * (copied through as NULL). Returns NULL only on the top-level allocation
 * failing; an individual string dup failing leaves that one field NULL
 * (matches this codebase's existing UseCases_buildRecord convention, e.g.
 * goose_subscriber_usecases.c).
 */
IpcMessage*
IpcDispatcherUseCases_assembleMessage(IpcSourceType sourceType, const char* sourceReference,
        bool hasBuffered, bool buffered, bool hasTimestamp, uint64_t timestampMs,
        const char* const* pointReferences, const IpcScalarValue* pointValues,
        const bool* pointHasQuality, const IpcQuality* pointQuality, int pointCount);

/* Frees an IpcMessage built above, including every data point's owned
 * reference/value string and the array itself. NULL-safe. */
void
IpcDispatcherUseCases_freeMessage(IpcMessage* message);

#endif /* IPC_DISPATCHER_USECASES_H_ */
