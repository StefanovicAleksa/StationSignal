#ifndef IED_MODEL_SCL_LOADER_H_
#define IED_MODEL_SCL_LOADER_H_

#include "iec61850_model.h"
#include "linked_list.h"
#include "features/ied_model/domain/ied_model_types.h"

/*
 * Parses the SCL file at `path` and builds a complete libiec61850 IedModel for
 * the <IED name=iedName> found in it, resolving the DataTypeTemplates join graph
 * (LNodeType -> DOType -> DAType/EnumType) and attaching DataSets, ReportControl
 * and GSEControl blocks declared in the instance section. GOOSE transport
 * addressing (VLAN/MAC/APPID) is attached only if a matching <Communication>
 * entry exists; its absence (typical for plain .icd files) is not an error.
 *
 * *outDaSemantics is set to NULL immediately, and (on success) reassigned to a
 * newly-created LinkedList of heap-boxed IedModelDaSemanticEntry* right before
 * returning - one entry per DataAttribute whose real SCL bType was genuinely
 * "Dbpos" (captured before IedModelUtils_mapBType collapses it into the
 * generic IEC61850_CODEDENUM type alongside "Tcmd" - see
 * IedModelDaSemantic's own doc comment). Caller owns the list and its
 * elements (LinkedList_destroyDeep(list, free)) - ied_model_api.c's
 * IedModel_loadFromFile copies each entry by value into the handle's own flat
 * array and then destroys this list.
 *
 * Returns NULL and sets *outError on failure. Caller owns the returned IedModel
 * (IedModel_destroy when done).
 */
IedModel*
IedModelSclLoader_load(const char* path, const char* iedName, IedModelLoadError* outError,
        LinkedList* outDaSemantics);

/*
 * Lists every direct-child <IED>'s name attribute at the SCL's top level
 * (IED elements are always direct children of the SCL root, never nested -
 * so this deliberately doesn't descend, unlike IedModelSclLoader_load's own
 * mxmlFindElement(..., MXML_DESCEND) lookup which searches for one specific
 * name anywhere below the root). Lighter than a full _load: no
 * DataTypeTemplates resolution, no IedModel construction at all.
 *
 * A file that parses fine but declares zero <IED> elements returns a valid,
 * non-NULL, empty list - that's not an error. NULL + *outError is reserved
 * for the same file/parse failures _load itself can hit
 * (IED_MODEL_ERR_FILE_NOT_FOUND / IED_MODEL_ERR_XML_PARSE) - never
 * IED_MODEL_ERR_IED_NOT_FOUND, which doesn't apply here since no specific
 * name is being looked up.
 *
 * Caller owns the returned list: LinkedList_destroyDeep(list, free).
 */
LinkedList
IedModelSclLoader_listIedNames(const char* path, IedModelLoadError* outError);

#endif /* IED_MODEL_SCL_LOADER_H_ */
