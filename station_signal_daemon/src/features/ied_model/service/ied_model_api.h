#ifndef IED_MODEL_API_H_
#define IED_MODEL_API_H_

#include "linked_list.h"
#include "mms_common.h"
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
 * Wraps an already-constructed, caller-owned dynamic IedModel (built via
 * iec61850_dynamic_model.h's own construction calls - IedModel_create,
 * LogicalDevice_create, LogicalNode_create, DataObject_create,
 * DataAttribute_create, DataSet_create/DataSetEntry_create,
 * ReportControlBlock_create, GSEControlBlock_create - e.g. by
 * ied_model_online_loader, which builds one from a live device's MMS ACSI
 * directory services instead of parsing an SCL file) in an IedModelHandle -
 * every accessor below then behaves identically regardless of whether the
 * model came from SCL parsing or live discovery, since they only ever walk
 * handle->model, never care how it was built. Ownership of `model` transfers
 * to the returned handle - IedModel_release destroys it exactly like a
 * loadFromFile'd one. Returns NULL (and never touches `model`) if model is
 * NULL or allocation fails.
 */
IedModelHandle
IedModel_wrapDynamicModel(IedModel* model, const char* iedName, AccessMode mode);

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
 * Index-aligned with IedModel_getDataSetMemberReferences's own result list
 * for the same datasetReference - element i is IED_MODEL_DA_SEMANTIC_NONE
 * unless raw dataset member i is already leaf-level (has an explicit
 * trailing daName in its FCDA) AND that terminal DataAttribute's real SCL
 * bType was genuinely "Dbpos" (see IedModelDaSemantic's own doc comment in
 * ied_model_types.h). Members that decompose (Gap 4 - a DO-level FCDA with
 * no daName) always report NONE here - use
 * IedModel_getDataSetMemberLeafSemantics for those instead, mirroring how
 * IedModel_getDataSetMemberLeafReferences is the decomposed-member
 * counterpart of getDataSetMemberReferences. Purely local (no over-the-wire
 * discovery), same as every other ied_model accessor. Always empty for a
 * model built via IedModel_wrapDynamicModel (no SCL bType available on that
 * path). Caller owns the list and its elements (heap-boxed
 * IedModelDaSemantic*): LinkedList_destroyDeep(list, free).
 */
LinkedList IedModel_getDataSetMemberSemantics(IedModelHandle handle, const char* datasetReference);

/*
 * Index-aligned with IedModel_getDataSetMemberLeafReferences's own result
 * list for the same (datasetReference, memberIndex) - same decomposition
 * rules, same "empty list if not decomposed" convention. Caller owns the
 * list and its elements (heap-boxed IedModelDaSemantic*):
 * LinkedList_destroyDeep(list, free).
 */
LinkedList IedModel_getDataSetMemberLeafSemantics(IedModelHandle handle, const char* datasetReference,
        int memberIndex);

/*
 * Index-aligned with IedModel_getDataSetMemberLeafReferences's own result
 * list for the same (datasetReference, memberIndex) - same decomposition
 * rules, same "empty list if not decomposed" convention. Each element is a
 * heap-boxed DataAttributeType: this leaf's own already-known SCL-declared
 * type, read directly off its DataAttribute node (set once at load time via
 * IedModelUtils_mapBType). Used by mms_report_client/goose_subscriber
 * alongside IedModel_dataAttributeTypeMatchesMmsType below to cross-check a
 * Gap-4 decomposition zip before trusting it - see that function's own doc
 * comment for the real-hardware finding this guards against. Empty for a
 * model built via IedModel_wrapDynamicModel (no SCL to have parsed a
 * DataAttributeType from).
 * Caller owns the list and its elements: LinkedList_destroyDeep(list, free).
 */
LinkedList IedModel_getDataSetMemberLeafWireTypes(IedModelHandle handle, const char* datasetReference,
        int memberIndex);

/*
 * Cross-checks one leaf's EXPECTED (SCL-declared) DataAttributeType (from
 * IedModel_getDataSetMemberLeafWireTypes) against its ACTUAL wire-decoded
 * MmsType (MmsValue_getType on the received value), before
 * mms_report_client/goose_subscriber trust a Gap-4 decomposition zip.
 * Exists because the pre-existing count-only fallback (see
 * getDataSetMemberLeafReferences's own doc comment on the wire-order
 * assumption) cannot catch a same-count-but-different-ORDER mismatch between
 * this daemon's locally-resolved leaf order (SCL <DOType> XML child order)
 * and a real device's actual runtime attribute order - confirmed against
 * real production hardware: a DPC's "Pos" structured attribute had its
 * stVal/t sub-elements zipped to the wrong reference labels (a UTC_TIME
 * value landed on the "stVal" reference, a BOOLEAN landed on "t") because
 * both orderings happened to have the same leaf count. Only implements
 * CONFIDENT, well-established groupings (per this codebase's "don't guess
 * IEC 61850 semantics" rule) - anything not explicitly modeled always
 * matches (no check), rather than risk a false-positive rejection of a
 * genuinely well-ordered structure.
 */
bool IedModel_dataAttributeTypeMatchesMmsType(DataAttributeType expected, MmsType actual);

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

/*
 * Whole-device counterpart of IedModel_getReportableAttributeReferencesForLogicalNode -
 * same FC=ST/MX "every leaf" convention and output format, but walks every LN
 * under every LD in the model instead of one caller-supplied LN. A "Dyn" RCB's
 * own parent LN does not restrict what a dataset assigned to it can report on
 * (dataset members are independently addressed per-element, not tied to any
 * one LN - confirmed against IedConnection_createDataSet's own wire format and
 * this codebase's own MmsReportClientUseCases_buildWireMemberReferences), so
 * mms_report_client uses this to cluster the ENTIRE device's reportable data
 * across however many spare "Dyn" RCB slots exist anywhere in the model, not
 * just slots parented on the same LN as the data. Purely local, never touches
 * the network. Available at IED_MODEL_ACCESS_REPORT_ONLY and above (i.e.
 * always). Caller owns the list and its elements:
 * LinkedList_destroyDeep(list, free). */
LinkedList IedModel_getReportableAttributeReferencesForWholeDevice(IedModelHandle handle);

/*
 * SCL's <Services><DynDataSet max="N" maxAttributes="M"/> for this IED - the
 * device's own declared dynamic-dataset-count budget and per-dataset
 * attribute cap, used by mms_report_client to budget/chunk dynamic dataset
 * creation instead of guessing. -1 means "not declared": no <Services>
 * element, a <Services/> with no <DynDataSet> child, that attribute missing
 * from a <DynDataSet> that IS present, or a model built via
 * IedModel_wrapDynamicModel (no <Services> is ever available over the wire).
 * 0 is a real, distinct value (device declares zero capacity) - never
 * conflated with -1. NULL handle also returns -1. Available at
 * IED_MODEL_ACCESS_REPORT_ONLY and above (i.e. always). Unlike every other
 * accessor in this header, these return a bare scalar, not a
 * LinkedList/bool/enum - nothing to free. */
int IedModel_getDynDataSetMax(IedModelHandle handle);
int IedModel_getDynDataSetMaxAttributes(IedModelHandle handle);

/*
 * Member-reference-keyed counterparts of IedModel_getDataSetMemberLeafReferences/
 * _getDataSetMemberLeafWireTypes/_getDataSetMemberLeafSemantics, plus a
 * single-member counterpart of _getDataSetMemberSemantics - resolved directly
 * from a "LD/LN$FC$DO[$DA]" member-reference string (the exact shape
 * IedModel_getDataSetMemberReferences/_getReportableAttributeReferencesForLogicalNode
 * already produce) instead of a (datasetReference, memberIndex) pair into a
 * locally-registered DataSet. Use these when the dataset in question was
 * resolved live over the wire (e.g. via IedConnection_getDataSetDirectory)
 * and has no corresponding DataSet object in this IedModel at all - purely
 * local tree lookups otherwise, same as every other accessor here. Same
 * "empty list / IED_MODEL_DA_SEMANTIC_NONE on any resolution failure, never
 * an error" contract as their DataSet-indexed counterparts. Caller owns each
 * returned list and its elements: LinkedList_destroyDeep(list, free).
 */
LinkedList IedModel_getLeafReferencesForMemberReference(IedModelHandle handle, const char* memberReference);
LinkedList IedModel_getLeafWireTypesForMemberReference(IedModelHandle handle, const char* memberReference);
LinkedList IedModel_getLeafSemanticsForMemberReference(IedModelHandle handle, const char* memberReference);
IedModelDaSemantic IedModel_getSemanticForMemberReference(IedModelHandle handle, const char* memberReference);

/* LinkedListValueDeleteFunction-compatible: frees a ReportControlBlockTarget. */
void IedModel_destroyReportControlBlockTarget(void* target);

/* LinkedListValueDeleteFunction-compatible: frees a GooseSubscriptionTarget. */
void IedModel_destroyGooseSubscriptionTarget(void* target);

/* Available at IED_MODEL_ACCESS_READ_ONLY and above; empty list otherwise. */
LinkedList IedModel_getReadTargets(IedModelHandle handle);

/* Available only at IED_MODEL_ACCESS_READ_AND_WRITE; empty list otherwise. */
LinkedList IedModel_getControlTargets(IedModelHandle handle);

#endif /* IED_MODEL_API_H_ */
