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
 * this INCLUDES "q" entries at their own original index) by ancestor prefix:
 *   - An entry whose daName == "q" is never itself emitted as a value entry
 *     (whether or not it has a matching sibling - a lone "q" is simply
 *     dropped, not fabricated into a value-less data point).
 *   - Every other entry (non-"q" daName, or an unparseable/NULL reference) is
 *     emitted as a value entry, in original index order, paired with a "q"
 *     sibling found by walking UP its own ancestor prefixes (one "$"-segment
 *     at a time) until an ancestor level with a "q" sibling is found, or none
 *     remain (-1). A flat attribute (e.g. "...Pos$stVal") finds its "q" one
 *     level up on the first try, same as a plain last-"$"-strip would; a
 *     deeply nested CONSTRUCTED-DA chain (e.g. a CMV's
 *     "...PhV$phsA$cVal$mag$f") walks past its own intermediate levels
 *     (neither "cVal$mag" nor "cVal" has its own "q") to find "...PhV$phsA$q"
 *     several segments up - quality belongs to the whole CDC instance, not to
 *     whichever BDA happens to be the terminal leaf of one nested DA within
 *     it. A single last-"$"-strip (this function's original implementation)
 *     only ever found quality for flat attributes, silently leaving every
 *     nested measured value (Dbpos aside, most measurands are CMV-shaped)
 *     with quality: null - confirmed against real production traffic.
 *
 * Writes into caller-supplied arrays (room for `count` elements each):
 *   outValueIndices[k]         = original index of the k-th value entry
 *   outQualityIndexForValue[k] = original index of that value's "q" sibling,
 *                                 or -1 if none
 * Returns the number of value entries written (0..count).
 *
 * O(count^2 * depth) (pairwise prefix comparison per ancestor level tried) -
 * fine for realistic dataset sizes (tens of entries, not thousands) and
 * shallow nesting (a handful of "$"-segments at most).
 */
int
IpcDispatcherUseCases_pairQuality(const char* const* references, int count,
        int* outValueIndices, int* outQualityIndexForValue);

/*
 * True if a value data point (already paired via IpcDispatcherUseCases_pairQuality
 * above) should actually reach the outbound dataPoints array, given whether ITS
 * OWN value individually changed and, if it has a paired quality sibling,
 * whether THAT quality individually changed:
 *   - valueOwnChangeDetected true -> always include (a genuine own change).
 *   - valueOwnChangeDetected false but hasQualityPair && qualityOwnChangeDetected
 *     -> still include (a genuine quality-only change must still surface its
 *     value sibling, or the change would have nowhere to attach in the JSON -
 *     preserves mms_report_client's existing, deliberately-symmetric
 *     group-extension behavior for this specific case).
 *   - otherwise -> exclude: this value is only present because a DIFFERENT
 *     sibling DA under the same DO/SDO changed (e.g. a DPC's "t"/"stSeld"
 *     riding along with an unrelated "stVal" change) - see
 *     mms_report_client_usecases.c's buildEntries doc comment for why that
 *     group membership itself is intentionally left untouched; this function
 *     is the ipc_dispatcher-side filter on top of it.
 */
bool
IpcDispatcherUseCases_shouldIncludeValuePoint(bool valueOwnChangeDetected, bool hasQualityPair,
        bool qualityOwnChangeDetected);

/*
 * Deep-copies already-paired, already-converted parallel arrays (each of
 * length pointCount) into one owned IpcMessage - does no further pairing;
 * the caller runs IpcDispatcherUseCases_pairQuality itself first, converts
 * the relevant MmsValues via utils/ipc_dispatcher_value_codec, then calls
 * this (see data/ adapters). Every string is deep-copied (pointReferences[k],
 * and pointValues[k]/extras->pointPreviousValue[k].value.str for STRING/RAW)
 * - caller's own arrays/strings may be freed immediately after this returns.
 * sourceReference may be NULL (copied through as NULL). extras may be NULL
 * (or any individual array within it may be NULL) - see IpcDataPointExtras'
 * own doc comment; label/previousLabel strings are copied BY POINTER, not
 * duplicated (static string-literal storage - see IpcDataPoint's own doc
 * comment), unlike every other string field here. Returns NULL only on the
 * top-level allocation failing; an individual string dup failing leaves that
 * one field NULL (matches this codebase's existing UseCases_buildRecord
 * convention, e.g. goose_subscriber_usecases.c).
 */
IpcMessage*
IpcDispatcherUseCases_assembleMessage(IpcSourceType sourceType, const char* sourceReference,
        bool hasBuffered, bool buffered, bool hasTimestamp, uint64_t timestampMs,
        const char* const* pointReferences, const IpcScalarValue* pointValues,
        const bool* pointHasQuality, const IpcQuality* pointQuality,
        const IpcDataPointExtras* extras, int pointCount);

/* Frees an IpcMessage built above, including every data point's owned
 * reference/value string and the array itself. NULL-safe. */
void
IpcDispatcherUseCases_freeMessage(IpcMessage* message);

#endif /* IPC_DISPATCHER_USECASES_H_ */
