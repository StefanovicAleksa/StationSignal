#ifndef IED_MODEL_USECASES_H_
#define IED_MODEL_USECASES_H_

#include "linked_list.h"
#include "features/ied_model/domain/ied_model_types.h"

/*
 * Pure logic over an already-built IedModel - no XML/mxml awareness at all here,
 * that's entirely the data layer's (ied_model_scl_loader) job.
 *
 * getReadTargets/getControlTargets each return a LinkedList of heap-allocated
 * char* object-reference strings; caller owns the list and its elements
 * (LinkedList_destroyDeep(list, free)).
 *
 * getReportSubscriptionTargets returns a LinkedList of heap-allocated
 * ReportControlBlockTarget* instead (buffered-vs-unbuffered and the dataset
 * reference matter to report consumers in a way plain read/control references
 * don't); caller owns the list and its elements
 * (LinkedList_destroyDeep(list, IedModelUseCases_destroyReportControlBlockTarget)).
 *
 * getGooseSubscriptionTargets returns a LinkedList of heap-allocated
 * GooseSubscriptionTarget* (object reference plus optional VLAN/APPID/dst-MAC
 * addressing parsed from SCL's <GSE><Address> block); caller owns the list and
 * its elements (LinkedList_destroyDeep(list,
 * IedModelUseCases_destroyGooseSubscriptionTarget)).
 *
 * AccessMode gating is NOT done here - these always compute the full result for
 * the model as built. The service layer (ied_model_api.c) decides which of these
 * to call based on the handle's AccessMode.
 */

LinkedList IedModelUseCases_getGooseSubscriptionTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getReportSubscriptionTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getReadTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getControlTargets(IedModelHandle handle);

/* LinkedListValueDeleteFunction-compatible: frees a ReportControlBlockTarget. */
void IedModelUseCases_destroyReportControlBlockTarget(void* target);

/* LinkedListValueDeleteFunction-compatible: frees a GooseSubscriptionTarget. */
void IedModelUseCases_destroyGooseSubscriptionTarget(void* target);

#endif /* IED_MODEL_USECASES_H_ */
