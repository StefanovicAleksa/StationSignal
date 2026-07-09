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

/*
 * Lists every <IED name="..."> declared at the top level of the SCL file at
 * `path`, without building a full model (no DataTypeTemplates resolution) -
 * lighter than IedModel_loadFromFile, and useful when the caller doesn't
 * already know the exact IED name to load (e.g. orchestration's optional
 * auto-detect: exactly one result means there's no ambiguity to resolve).
 * A file with zero <IED> elements returns a valid, non-NULL, empty list -
 * not an error. NULL + *outError only for file/parse failures. Caller owns
 * the list: LinkedList_destroyDeep(list, free).
 */
LinkedList
IedModel_listIedNames(const char* path, IedModelLoadError* outError);

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

/* Available at IED_MODEL_ACCESS_REPORT_ONLY and above (i.e. always) - purely
 * local, built from the already-parsed SCL DataSet, never touches the network
 * (see CLAUDE.md's "no over-the-wire tree discovery" rule). Returns a
 * LinkedList of heap-allocated char* member-reference strings, ordered to
 * match the dataset's own entry order. Caller owns the list and its elements:
 * LinkedList_destroyDeep(list, free). */
LinkedList IedModel_getDataSetMemberReferences(IedModelHandle handle, const char* datasetReference);

/*
 * For dataset member `memberIndex` of `datasetReference`: if that member's
 * FCDA omitted daName (the whole Data Object was included, not one leaf Data
 * Attribute), returns the ordered list of heap-allocated leaf-reference
 * char* strings for every terminal Data Attribute reachable under that DO at
 * its own FC (recursing through nested SDOs and CONSTRUCTED attributes) -
 * purely local, walks the already-parsed model, never touches the network.
 * Returns an empty (never NULL) list when the member is already leaf-level
 * or on any resolution failure - both mean "nothing to decompose, use the
 * plain reference from getDataSetMemberReferences as-is". See
 * IedModelUseCases_getDataSetMemberLeafReferences's own doc comment for the
 * wire-order assumption this relies on. Caller owns the list and its
 * elements: LinkedList_destroyDeep(list, free). */
LinkedList IedModel_getDataSetMemberLeafReferences(IedModelHandle handle, const char* datasetReference,
        int memberIndex);

/*
 * For one LN (given its own "LD/LN" object reference, e.g.
 * ReportControlBlockTarget.lnReference): returns every leaf Data Attribute at
 * FC=ST (status) or FC=MX (measurand) reachable under it, in the same
 * "LD/LN$FC$DO$DA" format IedModel_getDataSetMemberReferences already uses -
 * purely local, never touches the network (see CLAUDE.md's "no over-the-wire
 * tree discovery" rule). Used by mms_report_client to synthesize a dynamic
 * dataset's member list for an RCB whose SCL never declared one
 * (datSet="Dyn") - "all the variables" for that LN, by this codebase's
 * existing FC=ST/MX "reportable" convention (see IedModel_getReadTargets).
 * Available at IED_MODEL_ACCESS_REPORT_ONLY and above (i.e. always) - same
 * gating as getReportSubscriptionTargets/getDataSetMemberReferences. Returns
 * an empty (never NULL) list if lnReference is NULL or doesn't resolve.
 * Caller owns the list and its elements: LinkedList_destroyDeep(list, free). */
LinkedList IedModel_getReportableAttributeReferencesForLogicalNode(IedModelHandle handle, const char* lnReference);

/* LinkedListValueDeleteFunction-compatible: frees a ReportControlBlockTarget. */
void IedModel_destroyReportControlBlockTarget(void* target);

/* LinkedListValueDeleteFunction-compatible: frees a GooseSubscriptionTarget. */
void IedModel_destroyGooseSubscriptionTarget(void* target);

/* Available at IED_MODEL_ACCESS_READ_ONLY and above; empty list otherwise. */
LinkedList IedModel_getReadTargets(IedModelHandle handle);

/* Available only at IED_MODEL_ACCESS_READ_AND_WRITE; empty list otherwise. */
LinkedList IedModel_getControlTargets(IedModelHandle handle);

#endif /* IED_MODEL_API_H_ */
