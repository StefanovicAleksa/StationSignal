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

#endif /* GOOSE_SUBSCRIBER_UTILS_H_ */
