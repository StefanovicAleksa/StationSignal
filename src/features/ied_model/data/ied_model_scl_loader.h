#ifndef IED_MODEL_SCL_LOADER_H_
#define IED_MODEL_SCL_LOADER_H_

#include "iec61850_model.h"
#include "features/ied_model/domain/ied_model_types.h"

/*
 * Parses the SCL file at `path` and builds a complete libiec61850 IedModel for
 * the <IED name=iedName> found in it, resolving the DataTypeTemplates join graph
 * (LNodeType -> DOType -> DAType/EnumType) and attaching DataSets, ReportControl
 * and GSEControl blocks declared in the instance section. GOOSE transport
 * addressing (VLAN/MAC/APPID) is attached only if a matching <Communication>
 * entry exists; its absence (typical for plain .icd files) is not an error.
 *
 * Returns NULL and sets *outError on failure. Caller owns the returned IedModel
 * (IedModel_destroy when done).
 */
IedModel*
IedModelSclLoader_load(const char* path, const char* iedName, IedModelLoadError* outError);

#endif /* IED_MODEL_SCL_LOADER_H_ */
