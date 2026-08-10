#include <stdlib.h>
#include <string.h>
#include "features/ied_model/utils/ied_model_utils.h"

const char*
IedModelUtils_attrOrDefault(mxml_node_t* node, const char* attrName, const char* defaultValue) {
    const char* value = mxmlElementGetAttr(node, attrName);
    return value ? value : defaultValue;
}

const char*
IedModelUtils_attrRequired(mxml_node_t* node, const char* attrName) {
    return mxmlElementGetAttr(node, attrName);
}

bool
IedModelUtils_attrBool(mxml_node_t* node, const char* attrName, bool defaultValue) {
    const char* value = mxmlElementGetAttr(node, attrName);
    if (!value) return defaultValue;
    return (strcmp(value, "true") == 0) || (strcmp(value, "1") == 0);
}

int
IedModelUtils_attrInt(mxml_node_t* node, const char* attrName, int defaultValue) {
    const char* value = mxmlElementGetAttr(node, attrName);
    if (!value) return defaultValue;
    return atoi(value);
}

char*
IedModelUtils_buildLnName(const char* prefix, const char* lnClass, const char* inst) {
    size_t len = strlen(prefix) + strlen(lnClass) + strlen(inst) + 1;
    char* name = malloc(len);
    if (!name) return NULL;
    snprintf(name, len, "%s%s%s", prefix, lnClass, inst);
    return name;
}

DataAttributeType
IedModelUtils_mapBType(const char* bType) {
    if (strcmp(bType, "BOOLEAN") == 0) return IEC61850_BOOLEAN;
    if (strcmp(bType, "INT8") == 0) return IEC61850_INT8;
    if (strcmp(bType, "INT16") == 0) return IEC61850_INT16;
    /* Approximation: SCL's signed INT24 has no direct libiec61850 counterpart. */
    if (strcmp(bType, "INT24") == 0) return IEC61850_INT32;
    if (strcmp(bType, "INT32") == 0) return IEC61850_INT32;
    if (strcmp(bType, "INT64") == 0) return IEC61850_INT64;
    if (strcmp(bType, "INT128") == 0) return IEC61850_INT128;
    if (strcmp(bType, "INT8U") == 0) return IEC61850_INT8U;
    if (strcmp(bType, "INT16U") == 0) return IEC61850_INT16U;
    if (strcmp(bType, "INT24U") == 0) return IEC61850_INT24U;
    if (strcmp(bType, "INT32U") == 0) return IEC61850_INT32U;
    if (strcmp(bType, "FLOAT32") == 0) return IEC61850_FLOAT32;
    if (strcmp(bType, "FLOAT64") == 0) return IEC61850_FLOAT64;
    if (strcmp(bType, "Enum") == 0) return IEC61850_ENUMERATED;
    /* Dbpos/Tcmd are both 2-bit coded enumerations per IEC 61850-7-3, distinguished
     * only by attribute semantics, not representation. */
    if (strcmp(bType, "Dbpos") == 0) return IEC61850_CODEDENUM;
    if (strcmp(bType, "Tcmd") == 0) return IEC61850_CODEDENUM;
    if (strcmp(bType, "Quality") == 0) return IEC61850_QUALITY;
    if (strcmp(bType, "Timestamp") == 0) return IEC61850_TIMESTAMP;
    if (strcmp(bType, "VisString32") == 0) return IEC61850_VISIBLE_STRING_32;
    if (strcmp(bType, "VisString64") == 0) return IEC61850_VISIBLE_STRING_64;
    if (strcmp(bType, "VisString65") == 0) return IEC61850_VISIBLE_STRING_65;
    if (strcmp(bType, "VisString129") == 0) return IEC61850_VISIBLE_STRING_129;
    if (strcmp(bType, "VisString255") == 0) return IEC61850_VISIBLE_STRING_255;
    if (strcmp(bType, "Unicode255") == 0) return IEC61850_UNICODE_STRING_255;
    if (strcmp(bType, "Struct") == 0) return IEC61850_CONSTRUCTED;
    if (strcmp(bType, "EntryTime") == 0) return IEC61850_ENTRY_TIME;
    if (strcmp(bType, "PhyComAddr") == 0) return IEC61850_PHYCOMADDR;
    if (strcmp(bType, "TrgOps") == 0) return IEC61850_TRGOPS;
    if (strcmp(bType, "OptFlds") == 0) return IEC61850_OPTFLDS;
    if (strcmp(bType, "Check") == 0) return IEC61850_CHECK;
    /* Approximation: ObjRef has no direct counterpart; object references are
     * bounded strings per spec. */
    if (strcmp(bType, "ObjRef") == 0) return IEC61850_VISIBLE_STRING_129;
    if (strcmp(bType, "Currency") == 0) return IEC61850_CURRENCY;
    if (strcmp(bType, "Octet64") == 0) return IEC61850_OCTET_STRING_64;
    if (strcmp(bType, "Octet6") == 0) return IEC61850_OCTET_STRING_6;
    if (strcmp(bType, "Octet8") == 0) return IEC61850_OCTET_STRING_8;

    return IEC61850_UNKNOWN_TYPE;
}

uint8_t
IedModelUtils_buildTrgOps(mxml_node_t* trgOpsNode) {
    uint8_t trgOps = 0;
    if (!trgOpsNode) return trgOps;

    if (IedModelUtils_attrBool(trgOpsNode, "dchg", false)) trgOps |= TRG_OPT_DATA_CHANGED;
    if (IedModelUtils_attrBool(trgOpsNode, "qchg", false)) trgOps |= TRG_OPT_QUALITY_CHANGED;
    if (IedModelUtils_attrBool(trgOpsNode, "dupd", false)) trgOps |= TRG_OPT_DATA_UPDATE;
    if (IedModelUtils_attrBool(trgOpsNode, "period", false)) trgOps |= TRG_OPT_INTEGRITY;
    if (IedModelUtils_attrBool(trgOpsNode, "gi", false)) trgOps |= TRG_OPT_GI;

    return trgOps;
}

uint8_t
IedModelUtils_buildOptFlds(mxml_node_t* optFieldsNode) {
    uint8_t optFlds = 0;
    if (!optFieldsNode) return optFlds;

    if (IedModelUtils_attrBool(optFieldsNode, "seqNum", false)) optFlds |= RPT_OPT_SEQ_NUM;
    if (IedModelUtils_attrBool(optFieldsNode, "timeStamp", false)) optFlds |= RPT_OPT_TIME_STAMP;
    if (IedModelUtils_attrBool(optFieldsNode, "reasonCode", false)) optFlds |= RPT_OPT_REASON_FOR_INCLUSION;
    if (IedModelUtils_attrBool(optFieldsNode, "dataSet", false)) optFlds |= RPT_OPT_DATA_SET;
    if (IedModelUtils_attrBool(optFieldsNode, "dataRef", false)) optFlds |= RPT_OPT_DATA_REFERENCE;
    if (IedModelUtils_attrBool(optFieldsNode, "bufOvfl", false)) optFlds |= RPT_OPT_BUFFER_OVERFLOW;
    if (IedModelUtils_attrBool(optFieldsNode, "entryID", false)) optFlds |= RPT_OPT_ENTRY_ID;
    if (IedModelUtils_attrBool(optFieldsNode, "configRef", false)) optFlds |= RPT_OPT_CONF_REV;

    return optFlds;
}

/*
 * Rewrites every '.' in [src, srcEnd) to '$' while copying into dst.
 *
 * IEC 61850-6 lets an FCDA's doName/daName carry a DOTTED PATH rather than a
 * single name - doName="SeqA.c3" is the SDO chain SeqA -> c3, daName="mag.f"
 * the BDA chain mag -> f - whose wire form is "$"-joined like every other
 * segment of the reference. Real ABB station files use both forms (24 dotted
 * doName + 12 dotted daName in one 48-IED file, in its MeasFltA and Goose
 * datasets); Siemens' own exports never do, which is why this went unnoticed.
 *
 * Left un-expanded, the resulting entry read "...$MX$SeqA.c3" - the dataset
 * entry was still created, so no wire position shifted and no OTHER member was
 * mislabeled, but that one member carried a reference no consumer could match
 * and silently lost Gap-4 decomposition, Dbpos semantics and desc lookup, all
 * of which resolve by walking "$" segments.
 *
 * A '.' is never legal INSIDE a single SCL name (tRestrName is
 * [A-Za-z][0-9A-Za-z_]*), so an unconditional rewrite can't corrupt a
 * non-dotted name - there is nothing to distinguish, hence no flag or
 * lookahead here.
 */
static char*
appendWithDotsAsSeparators(char* dst, const char* src) {
    for (; *src; src++) {
        *dst++ = (*src == '.') ? '$' : *src;
    }
    return dst;
}

char*
IedModelUtils_buildFcdaVariableName(const char* ldName, const char* lnClass, const char* lnInst,
        const char* prefix, const char* fc, const char* doName, const char* daName) {
    char* lnName = IedModelUtils_buildLnName(prefix ? prefix : "", lnClass, lnInst ? lnInst : "");
    if (!lnName) return NULL;

    /* Unchanged: the dot rewrite above is length-preserving (one char in, one
     * char out), so this sizing still holds exactly. */
    size_t len = strlen(ldName) + 1 + strlen(lnName) + 1 + strlen(fc) + 1 + strlen(doName) + 1
            + (daName ? strlen(daName) : 0) + 1;
    char* variable = malloc(len);
    if (!variable) {
        free(lnName);
        return NULL;
    }

    /* Built by hand rather than snprintf'd so doName/daName can be copied
     * through the dot rewrite. Applied uniformly to every part for one code
     * path rather than two: ldName/lnName/fc are all tRestrName-shaped and
     * can never contain a '.', so it is a plain copy for them. */
    char* cursor = variable;
    cursor = appendWithDotsAsSeparators(cursor, ldName);
    *cursor++ = '/';
    cursor = appendWithDotsAsSeparators(cursor, lnName);
    *cursor++ = '$';
    cursor = appendWithDotsAsSeparators(cursor, fc);
    *cursor++ = '$';
    cursor = appendWithDotsAsSeparators(cursor, doName);
    if (daName) {
        *cursor++ = '$';
        cursor = appendWithDotsAsSeparators(cursor, daName);
    }
    *cursor = '\0';

    free(lnName);
    return variable;
}
