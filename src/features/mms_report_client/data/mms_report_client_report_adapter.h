#ifndef MMS_REPORT_CLIENT_REPORT_ADAPTER_H_
#define MMS_REPORT_CLIENT_REPORT_ADAPTER_H_

#include "iec61850_client.h"
#include "features/mms_report_client/domain/mms_report_client_types.h"

/*
 * The ONLY file in this feature that touches the opaque ClientReport type.
 * Installed as the ReportCallbackFunction on IedConnection (see
 * mms_report_client_connection.c). Extracts and deep-copies every field it
 * needs before returning - ClientReport_getDataSetValues/getDataReference/
 * getEntryId are documented (iec61850_client.h) as only valid for the
 * lifetime of the ClientReport / inside this callback - then hands an owned
 * MmsReportRecord (built via mms_report_client_usecases) to the user's
 * registered report callback.
 *
 * Fires on libiec61850's internal reception thread (IedConnection_createEx(NULL, true)).
 * Must not block, must not call install/uninstallReportHandler or any
 * synchronous IedConnection_* function from here (deadlock risk - see
 * ReportCallbackFunction's documented constraints).
 */
void
MmsReportClientReportAdapter_onReport(void* parameter, ClientReport report);

#endif /* MMS_REPORT_CLIENT_REPORT_ADAPTER_H_ */
