#ifndef IED_MODEL_USECASES_H_
#define IED_MODEL_USECASES_H_

#include "linked_list.h"
#include "mms_common.h"
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
 * Returns EVERY RCB regardless of handle->categoryFilter - deliberately not
 * gated by category (see this function's own doc comment in the .c file):
 * the daemon always needs every RCB visible to know where every dataset
 * lives. Category filtering happens downstream, per data point.
 *
 * getGooseSubscriptionTargets returns a LinkedList of heap-allocated
 * GooseSubscriptionTarget* (object reference plus optional VLAN/APPID/dst-MAC
 * addressing parsed from SCL's <GSE><Address> block); caller owns the list and
 * its elements (LinkedList_destroyDeep(list,
 * IedModelUseCases_destroyGooseSubscriptionTarget)). Same category-blind
 * contract as getReportSubscriptionTargets above.
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
 *
 * getReportableAttributeReferencesForWholeDevice: same FC=ST/MX "every leaf"
 * convention and output format as getReportableAttributeReferencesForLogicalNode,
 * but walks every LN under every LD in the model instead of one caller-supplied
 * LN - used by mms_report_client's whole-device dynamic-dataset clustering,
 * since a "Dyn" RCB's own parent LN does not restrict what a dataset assigned
 * to it can report on (a dataset's members are independently addressed, not
 * tied to the RCB's LN). Purely local, never touches the network. Caller owns
 * the list and its elements (LinkedList_destroyDeep(list, free)).
 *
 * getDataSetMemberLeafWireTypes: index-aligned with
 * getDataSetMemberLeafReferences's own result list for the same
 * (datasetReference, memberIndex) - same decomposition rules, same
 * "empty list if not decomposed" convention. Each element is a heap-boxed
 * DataAttributeType (this leaf's own already-known SCL-declared type, read
 * directly off its DataAttribute node) - see
 * dataAttributeTypeMatchesMmsType's own doc comment for what this is used
 * for. Caller owns the list and its elements: LinkedList_destroyDeep(list, free).
 */

LinkedList IedModelUseCases_getGooseSubscriptionTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getReportSubscriptionTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getReadTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getControlTargets(IedModelHandle handle);
LinkedList IedModelUseCases_getDataSetMemberReferences(IedModelHandle handle, const char* datasetReference);
LinkedList IedModelUseCases_getDataSetMemberLeafReferences(IedModelHandle handle, const char* datasetReference,
        int memberIndex);
LinkedList IedModelUseCases_getDataSetMemberSemantics(IedModelHandle handle, const char* datasetReference);
LinkedList IedModelUseCases_getDataSetMemberLeafSemantics(IedModelHandle handle, const char* datasetReference,
        int memberIndex);
LinkedList IedModelUseCases_getDataSetMemberLeafWireTypes(IedModelHandle handle, const char* datasetReference,
        int memberIndex);
LinkedList IedModelUseCases_getReportableAttributeReferencesForLogicalNode(IedModelHandle handle,
        const char* lnReference);
LinkedList IedModelUseCases_getReportableAttributeReferencesForWholeDevice(IedModelHandle handle);

/* SCL's <Services><DynDataSet max="N" maxAttributes="M"/> for this IED. -1 if
 * not declared (see sIedModelHandle's own field doc comment); NULL handle
 * also returns -1. */
int IedModelUseCases_getDynDataSetMax(IedModelHandle handle);
int IedModelUseCases_getDynDataSetMaxAttributes(IedModelHandle handle);

/* Same shape as above, for the sibling <ConfDataSet max="N" maxAttributes="M"/>
 * - the domain-scoped ("Conf") dataset pool, distinct from DynDataSet's
 * association-scoped ("Dyn") one. */
int IedModelUseCases_getConfDataSetMax(IedModelHandle handle);
int IedModelUseCases_getConfDataSetMaxAttributes(IedModelHandle handle);

/* This handle's active category filter mask (see sIedModelHandle.categoryFilter's
 * own doc comment) - mms_report_client/goose_subscriber read this once per
 * connection to cache it alongside each dataset member's own resolved
 * category, for per-data-point filtering. NULL handle returns
 * IED_MODEL_LN_CATEGORY_ALL (unfiltered), the same safe default the field
 * itself defaults to. */
LnCategoryMask IedModelUseCases_getCategoryFilter(IedModelHandle handle);

/*
 * Member-reference-keyed counterparts of getDataSetMemberLeafReferences/
 * _getDataSetMemberLeafWireTypes/_getDataSetMemberSemantics(one entry)/
 * _getDataSetMemberLeafSemantics - resolved directly from a "LD/LN$FC$DO[$DA]"
 * member-reference string (this codebase's own convention - the exact shape
 * getDataSetMemberReferences/getReportableAttributeReferencesForLogicalNode
 * already produce) instead of a (datasetReference, memberIndex) pair into a
 * DataSet registered in this model. Needed because a dataset resolved live
 * over the wire (e.g. mms_report_client's tier-2 "pulled" live-assigned
 * dataset, see that feature's own doc comments) has no DataSet/DataSetEntry
 * object registered in this IedModel at all - the DataSet-indexed accessors
 * above are now thin wrappers around these. Same contracts as their
 * DataSet-indexed counterparts: empty list / IED_MODEL_DA_SEMANTIC_NONE on
 * any resolution failure, never an error.
 */
LinkedList IedModelUseCases_getLeafReferencesForMemberReference(IedModelHandle handle, const char* memberReference);
LinkedList IedModelUseCases_getLeafWireTypesForMemberReference(IedModelHandle handle, const char* memberReference);
LinkedList IedModelUseCases_getLeafSemanticsForMemberReference(IedModelHandle handle, const char* memberReference);
IedModelDaSemantic IedModelUseCases_getSemanticForMemberReference(IedModelHandle handle, const char* memberReference);

/*
 * LN-level counterpart of getSemanticForMemberReference - unlike a Dbpos
 * semantic (genuinely per-DA), LnCategory is constant across every leaf
 * under one LN (see IedModelLnCategoryEntry's own doc comment), so this
 * resolves only as far as the reference's own "LD/LN" prefix, never down to
 * a terminal DataAttribute. Returns IED_MODEL_LN_CATEGORY_OTHER (never a
 * guess into a real category) if memberReference is NULL, malformed, or its
 * LN doesn't resolve in this model.
 */
LnCategory IedModelUseCases_getCategoryForMemberReference(IedModelHandle handle, const char* memberReference);

/*
 * Description-lookup counterparts of getSemanticForMemberReference/
 * getLeafSemanticsForMemberReference - same resolution shape, but return the
 * SCL desc="..." string captured for that leaf (IedModelDaDescEntry, DA-level
 * or DAI-instance-level, whichever a handle ends up with per its own doc
 * comment) instead of an IedModelDaSemantic. Returned string(s) are
 * BORROWED (point directly into handle->daDescriptions - owned by the handle,
 * not the caller) - never free them; getDescriptionForMemberReference returns
 * NULL if no description was captured for that leaf (or on any resolution
 * failure, same graceful-degradation posture as every other lookup here).
 * getLeafDescriptionsForMemberReference's list is index-aligned with
 * getLeafReferencesForMemberReference's own output for the same
 * memberReference - a NULL element means "this leaf has no description," not
 * an error; the caller owns the list structure only
 * (LinkedList_destroyStatic), never the strings inside it.
 */
const char* IedModelUseCases_getDescriptionForMemberReference(IedModelHandle handle, const char* memberReference);
LinkedList IedModelUseCases_getLeafDescriptionsForMemberReference(IedModelHandle handle, const char* memberReference);

/*
 * Cross-checks one leaf's EXPECTED (SCL-declared) DataAttributeType against
 * its ACTUAL wire-decoded MmsType - see the .c file's own doc comment on this
 * function for the full real-hardware finding this guards against. Only
 * implements confident, well-established groupings; anything not explicitly
 * modeled always matches (no check).
 */
bool IedModelUseCases_dataAttributeTypeMatchesMmsType(DataAttributeType expected, MmsType actual);

/* LinkedListValueDeleteFunction-compatible: frees a ReportControlBlockTarget. */
void IedModelUseCases_destroyReportControlBlockTarget(void* target);

/* LinkedListValueDeleteFunction-compatible: frees a GooseSubscriptionTarget. */
void IedModelUseCases_destroyGooseSubscriptionTarget(void* target);

#endif /* IED_MODEL_USECASES_H_ */
