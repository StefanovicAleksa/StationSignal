#ifndef GOOSE_SUBSCRIBER_UTILS_H_
#define GOOSE_SUBSCRIBER_UTILS_H_

#include "mms_value.h"

/*
 * Small reusable third-party (MmsValue) helpers shared by the frame adapter
 * (data/) and the usecases layer (domain/). No GooseSubscriber/GooseReceiver
 * awareness here - just MmsValue/plain-C-string cloning. Same shape as
 * mms_report_client_utils, minus a ReasonForInclusion helper - GOOSE has no
 * per-entry reason-for-inclusion concept (the whole dataset is retransmitted
 * on every message, unlike an MMS report's per-element change tracking).
 */

/*
 * Clones each of the first `count` elements of a MMS_ARRAY MmsValue into a
 * freshly allocated array of owned MmsValue* pointers (via MmsValue_clone).
 * Caller owns the returned array and every element (MmsValue_delete each,
 * then free the array). Returns NULL if dataSetValues is NULL or count <= 0.
 */
MmsValue**
GooseSubscriberUtils_cloneMmsValueArray(const MmsValue* dataSetValues, int count);

/* NULL-safe strdup - returns NULL if s is NULL. Caller owns the result (free). */
char*
GooseSubscriberUtils_safeStringDup(const char* s);

/*
 * Recursively flattens a (possibly nested) MMS_STRUCTURE MmsValue into an
 * ordered array of its terminal (non-structure) leaf elements - a DO-level
 * dataset entry (FCDA with no daName) arrives as one such structure (e.g.
 * {stVal, q, t}, or several levels deep for CDCs like WYE->CMV->Vector).
 * Visitation is depth-first in element order, matching the same convention
 * IedModel_getDataSetMemberLeafReferences uses over the DOType/DAType model
 * tree - correspondence between the two is purely positional, an assumption
 * documented on that function, not verified by type here.
 *
 * If `value` is not itself MMS_STRUCTURE, returns a single-element array
 * containing `value` unchanged - nothing to flatten. Returned pointers are
 * BORROWED from `value`, never cloned - caller must MmsValue_clone before
 * value's owning GooseSubscriber becomes invalid. Caller owns only the
 * returned array (free it), never its elements. Returns NULL (with
 * *outLeafCount left at 0) if `value` is NULL or on allocation failure.
 */
MmsValue**
GooseSubscriberUtils_flattenStructure(const MmsValue* value, int* outLeafCount);

#endif /* GOOSE_SUBSCRIBER_UTILS_H_ */
