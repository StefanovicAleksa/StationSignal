#ifndef IED_MODEL_API_H_
#define IED_MODEL_API_H_

#include "linked_list.h"
#include "features/ied_model/domain/ied_model_types.h"

/*
 * Public boundary of the ied_model feature. Other features (goose_subscriber,
 * mms_report_client, ...) should only ever include this header - never reach
 * into domain/data/utils directly.
 */

/* Returns NULL and sets *outError on failure. Caller owns the handle (IedModel_release). */
IedModelHandle
IedModel_loadFromFile(const char* path, const char* iedName, AccessMode mode, IedModelLoadError* outError);

void
IedModel_release(IedModelHandle handle);

/*
 * getReadTargets/getControlTargets each return a LinkedList of heap-allocated
 * char* object-reference strings. Caller owns the list and its elements:
 * LinkedList_destroyDeep(list, free).
 *
 * getReportSubscriptionTargets returns a LinkedList of heap-allocated
 * ReportControlBlockTarget* instead. Caller owns the list and its elements:
 * LinkedList_destroyDeep(list, IedModel_destroyReportControlBlockTarget).
 *
 * getGooseSubscriptionTargets returns a LinkedList of heap-allocated
 * GooseSubscriptionTarget* (object reference plus optional VLAN/APPID/dst-MAC
 * addressing parsed from SCL). Caller owns the list and its elements:
 * LinkedList_destroyDeep(list, IedModel_destroyGooseSubscriptionTarget).
 */

/* Available at IED_MODEL_ACCESS_REPORT_ONLY and above (i.e. always). */
LinkedList IedModel_getGooseSubscriptionTargets(IedModelHandle handle);
LinkedList IedModel_getReportSubscriptionTargets(IedModelHandle handle);

/* LinkedListValueDeleteFunction-compatible: frees a ReportControlBlockTarget. */
void IedModel_destroyReportControlBlockTarget(void* target);

/* LinkedListValueDeleteFunction-compatible: frees a GooseSubscriptionTarget. */
void IedModel_destroyGooseSubscriptionTarget(void* target);

/* Available at IED_MODEL_ACCESS_READ_ONLY and above; empty list otherwise. */
LinkedList IedModel_getReadTargets(IedModelHandle handle);

/* Available only at IED_MODEL_ACCESS_READ_AND_WRITE; empty list otherwise. */
LinkedList IedModel_getControlTargets(IedModelHandle handle);

#endif /* IED_MODEL_API_H_ */
