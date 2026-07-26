#ifndef IED_MODEL_UTILS_H_
#define IED_MODEL_UTILS_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "mxml.h"
#include "iec61850_model.h"
#include "iec61850_common.h"

/* Returns the attribute value, or defaultValue if the attribute is absent. Never NULL. */
const char*
IedModelUtils_attrOrDefault(mxml_node_t* node, const char* attrName, const char* defaultValue);

/* Returns the attribute value, or NULL if the attribute is absent (caller must check). */
const char*
IedModelUtils_attrRequired(mxml_node_t* node, const char* attrName);

bool
IedModelUtils_attrBool(mxml_node_t* node, const char* attrName, bool defaultValue);

int
IedModelUtils_attrInt(mxml_node_t* node, const char* attrName, int defaultValue);

/*
 * Synthesizes the IEC 61850 instance name of a logical node: prefix + lnClass + inst.
 * For LN0, prefix/inst are always empty strings, giving "LLN0". Caller owns the
 * returned buffer (free() it).
 */
char*
IedModelUtils_buildLnName(const char* prefix, const char* lnClass, const char* inst);

/*
 * Maps an SCL bType attribute string (IEC 61850-6 tBasicType) to the closest
 * libiec61850 DataAttributeType. Two approximations, both commented at the call
 * site in the .c file: INT24 (signed) has no direct enum counterpart and maps to
 * IEC61850_INT32; ObjRef has no direct counterpart and maps to
 * IEC61850_VISIBLE_STRING_129 (object references are bounded strings per spec).
 * Returns IEC61850_UNKNOWN_TYPE for anything unrecognized - caller must check.
 */
DataAttributeType
IedModelUtils_mapBType(const char* bType);

/* Builds the TRG_OPT_* bitset from a <TrgOps> element's boolean attributes. */
uint8_t
IedModelUtils_buildTrgOps(mxml_node_t* trgOpsNode);

/* Builds the RPT_OPT_* bitset from an <OptFields> element's boolean attributes. */
uint8_t
IedModelUtils_buildOptFlds(mxml_node_t* optFieldsNode);

/*
 * Builds the "$"-joined MMS variable name string DataSetEntry_create expects,
 * e.g. "ied1Inverter/LLN0$ST$Mod$q", from an FCDA's attributes. ldName is the
 * already-resolved wire-form logical device name (IED name + ldInst). daName
 * may be NULL to reference the whole data object. Caller owns the returned
 * buffer (free() it).
 */
char*
IedModelUtils_buildFcdaVariableName(const char* ldName, const char* lnClass, const char* lnInst,
        const char* prefix, const char* fc, const char* doName, const char* daName);

#endif /* IED_MODEL_UTILS_H_ */
