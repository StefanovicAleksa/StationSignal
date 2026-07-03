#ifndef MMS_REPORT_CLIENT_UTILS_H_
#define MMS_REPORT_CLIENT_UTILS_H_

#include "mms_value.h"
#include "features/mms_report_client/domain/mms_report_client_types.h"

/*
 * Small reusable third-party (MmsValue) helpers shared by the report adapter
 * (data/) and the usecases layer (domain/). No ClientReport/IedConnection
 * awareness here - just MmsValue/plain-C-array cloning.
 */

/*
 * Clones each of the first `count` elements of a MMS_ARRAY/MMS_STRUCTURE
 * MmsValue into a freshly allocated array of owned MmsValue* pointers (via
 * MmsValue_clone). Caller owns the returned array and every element
 * (MmsValue_delete each, then free the array). Returns NULL if dataSetValues
 * is NULL or count <= 0.
 */
MmsValue**
MmsReportClientUtils_cloneMmsValueArray(const MmsValue* dataSetValues, int count);

/*
 * Clones a plain ReasonForInclusion array. Caller owns the returned array
 * (free it). Returns NULL if src is NULL or count <= 0.
 */
ReasonForInclusion*
MmsReportClientUtils_cloneReasonArray(const ReasonForInclusion* src, int count);

/* NULL-safe strdup - returns NULL if s is NULL. Caller owns the result (free). */
char*
MmsReportClientUtils_safeStringDup(const char* s);

#endif /* MMS_REPORT_CLIENT_UTILS_H_ */
