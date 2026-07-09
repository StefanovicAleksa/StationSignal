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
 *
 * getDataSetMemberReferences returns an ordered LinkedList of heap-allocated
 * char* member-reference strings for one dataset (index i matches the i-th
 * DataSetEntry / MmsReportEntry[i] / GooseSubscriberEntry[i] built from that
 * same dataset). Returns an empty (never NULL) list if datasetReference is
 * NULL or doesn't resolve. Caller owns the list and its elements
 * (LinkedList_destroyDeep(list, free)).
 *
 * getDataSetMemberLeafReferences: for dataset member `memberIndex`, if its
 * FCDA omitted daName (the whole Data Object was included, not one leaf Data
 * Attribute - see CLAUDE.md), returns the ordered list of heap-allocated
 * leaf-reference char* strings for every genuinely terminal (basic-typed)
 * Data Attribute reachable under that DO at its own FC - recursing through
 * nested SDOs and through a CONSTRUCTED Data Attribute's own BDA children
 * too (real vendor CDCs like WYE->CMV->Vector->AnalogueValue nest several
 * levels deep - see collectLeafReferencesByFc's own comment for why this is
 * NOT the same walk as getReadTargets' collectDataAttributesByFc, which
 * deliberately stops at the DA level). Order matches this function's own
 * depth-first traversal; callers rely on this matching the wire's own
 * MMS_STRUCTURE element order for that entry, which is NOT spec-guaranteed
 * in libiec61850's headers - treat as an assumption, not a certainty (see
 * mms_report_client/goose_subscriber's own decomposition code for the
 * defensive count-mismatch fallback this relies on). Returns an empty (never
 * NULL) list when the member is already leaf-level (daName was present) or
 * on any resolution failure - both mean "nothing to decompose, use the plain
 * reference as-is". Caller owns the list and its elements
 * (LinkedList_destroyDeep(list, free)).
 *
 * getReportableAttributeReferencesForLogicalNode: for one LN (given its own
 * "LD/LN" object reference, e.g. ReportControlBlockTarget.lnReference),
 * returns every leaf Data Attribute at FC=ST or FC=MX reachable under it, in
 * the same "LD/LN$FC$DO$DA" format getDataSetMemberReferences already uses -
 * used by mms_report_client to synthesize a dynamic dataset's member list for
 * an RCB whose SCL never declared one (datSet="Dyn"). Purely local, never
 * touches the network. Returns an empty (never NULL) list if lnReference is
 * NULL or doesn't resolve. Caller owns the list and its elements
 * (LinkedList_destroyDeep(list, free)).
 */

LinkedList IedModelUseCases_getGooseSubscriptionTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getReportSubscriptionTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getReadTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getControlTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getDataSetMemberReferences(IedModelHandle handle, const char* datasetReference);
LinkedList IedModelUseCases_getDataSetMemberLeafReferences(IedModelHandle handle, const char* datasetReference,
        int memberIndex);
LinkedList IedModelUseCases_getReportableAttributeReferencesForLogicalNode(IedModelHandle handle,
        const char* lnReference);

/* LinkedListValueDeleteFunction-compatible: frees a ReportControlBlockTarget. */
void IedModelUseCases_destroyReportControlBlockTarget(void* target);

/* LinkedListValueDeleteFunction-compatible: frees a GooseSubscriptionTarget. */
void IedModelUseCases_destroyGooseSubscriptionTarget(void* target);

#endif /* IED_MODEL_USECASES_H_ */
