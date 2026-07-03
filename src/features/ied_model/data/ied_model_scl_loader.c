#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model/data/ied_model_scl_loader.h"
#include "features/ied_model/utils/ied_model_utils.h"
#include "iec61850_dynamic_model.h"
#include "mms_value.h"
#include "mxml.h"

typedef struct {
    IedModel* model;
    mxml_node_t* sclRoot;
    mxml_node_t* templates; /* may be NULL - every lookup against it must handle that */
    const char* iedName;
} LoaderContext;

/* ---- generic mxml element-only child/sibling iteration ---- */

static mxml_node_t*
firstElementChild(mxml_node_t* parent) {
    mxml_node_t* child = mxmlGetFirstChild(parent);
    while (child && mxmlGetType(child) != MXML_ELEMENT) child = mxmlGetNextSibling(child);
    return child;
}

static mxml_node_t*
nextElementSibling(mxml_node_t* node) {
    mxml_node_t* sib = mxmlGetNextSibling(node);
    while (sib && mxmlGetType(sib) != MXML_ELEMENT) sib = mxmlGetNextSibling(sib);
    return sib;
}

static bool
isElement(mxml_node_t* node, const char* name) {
    return node && mxmlGetType(node) == MXML_ELEMENT && strcmp(mxmlGetElement(node), name) == 0;
}

static mxml_node_t*
findFirstChildElement(mxml_node_t* parent, const char* name) {
    for (mxml_node_t* c = firstElementChild(parent); c; c = nextElementSibling(c)) {
        if (isElement(c, name)) return c;
    }
    return NULL;
}

/* ---- DataTypeTemplates lookups (flat sibling collections, joined by string id) ---- */

static mxml_node_t*
findTemplateById(mxml_node_t* templates, const char* tag, const char* id) {
    if (!templates || !id) return NULL;
    return mxmlFindElement(templates, templates, tag, "id", id, MXML_DESCEND);
}

static FunctionalConstraint
resolveFc(const char* fcStr) {
    if (!fcStr) {
        fprintf(stderr, "[ied_model] WARN: data attribute missing fc, defaulting to NONE\n");
        return IEC61850_FC_NONE;
    }
    return FunctionalConstraint_fromString(fcStr);
}

/* ---- recursive DataObject/DataAttribute construction from DataTypeTemplates ---- */

static void buildDataObject(const char* name, const char* doTypeId, ModelNode* parent, mxml_node_t* templates);

static void
buildDataAttribute(mxml_node_t* node, ModelNode* parent, const char* inheritedFc, mxml_node_t* templates) {
    const char* name = IedModelUtils_attrRequired(node, "name");
    const char* bType = IedModelUtils_attrRequired(node, "bType");
    if (!name || !bType) {
        fprintf(stderr, "[ied_model] WARN: skipping malformed DA/BDA (missing name or bType)\n");
        return;
    }

    const char* fcStr = IedModelUtils_attrOrDefault(node, "fc", inheritedFc);
    FunctionalConstraint fc = resolveFc(fcStr);
    uint8_t trgOps = IedModelUtils_buildTrgOps(node);
    DataAttributeType type = IedModelUtils_mapBType(bType);

    if (type == IEC61850_UNKNOWN_TYPE) {
        fprintf(stderr, "[ied_model] WARN: unmapped bType '%s' for attribute '%s', skipping\n", bType, name);
        return;
    }

    DataAttribute* da = DataAttribute_create(name, parent, type, fc, trgOps, 0, 0);

    if (type == IEC61850_CONSTRUCTED) {
        const char* typeId = IedModelUtils_attrRequired(node, "type");
        mxml_node_t* daType = findTemplateById(templates, "DAType", typeId);
        if (!daType) {
            fprintf(stderr, "[ied_model] WARN: unresolved DAType '%s' for attribute '%s'\n",
                    typeId ? typeId : "(missing)", name);
            return;
        }
        for (mxml_node_t* bda = firstElementChild(daType); bda; bda = nextElementSibling(bda)) {
            if (isElement(bda, "BDA")) {
                buildDataAttribute(bda, (ModelNode*) da, fcStr, templates);
            }
        }
    }
}

static void
buildDataObject(const char* name, const char* doTypeId, ModelNode* parent, mxml_node_t* templates) {
    DataObject* dobj = DataObject_create(name, parent, 0);

    mxml_node_t* doType = findTemplateById(templates, "DOType", doTypeId);
    if (!doType) {
        fprintf(stderr, "[ied_model] WARN: unresolved DOType '%s' for data object '%s'\n",
                doTypeId ? doTypeId : "(missing)", name);
        return;
    }

    for (mxml_node_t* child = firstElementChild(doType); child; child = nextElementSibling(child)) {
        if (isElement(child, "DA")) {
            buildDataAttribute(child, (ModelNode*) dobj, NULL, templates);
        } else if (isElement(child, "SDO")) {
            const char* sdoName = IedModelUtils_attrRequired(child, "name");
            const char* sdoTypeId = IedModelUtils_attrRequired(child, "type");
            if (!sdoName || !sdoTypeId) {
                fprintf(stderr, "[ied_model] WARN: skipping malformed SDO under DOType '%s'\n", doTypeId);
                continue;
            }
            buildDataObject(sdoName, sdoTypeId, (ModelNode*) dobj, templates);
        }
    }
}

/* ---- DOI/DAI/Val default-value overrides ---- */

static void
applyValueOverride(DataAttribute* da, const char* valText) {
    MmsValue* value = NULL;

    switch (da->type) {
        case IEC61850_BOOLEAN:
            value = MmsValue_newBoolean(strcmp(valText, "true") == 0 || strcmp(valText, "1") == 0);
            break;
        case IEC61850_INT8:
        case IEC61850_INT16:
        case IEC61850_INT32:
        case IEC61850_INT64:
            value = MmsValue_newIntegerFromInt32((int32_t) atoi(valText));
            break;
        case IEC61850_INT8U:
        case IEC61850_INT16U:
        case IEC61850_INT24U:
        case IEC61850_INT32U:
            value = MmsValue_newUnsignedFromUint32((uint32_t) strtoul(valText, NULL, 10));
            break;
        case IEC61850_FLOAT32:
        case IEC61850_FLOAT64:
            value = MmsValue_newFloat((float) atof(valText));
            break;
        case IEC61850_ENUMERATED:
            /* Val text here is the EnumType label (e.g. "status-only"); ordinal lookup
             * against <EnumType> isn't implemented yet, so this only works correctly if
             * the file provides a numeric ordinal directly. Flagged as a follow-up rather
             * than guessed at. */
            value = MmsValue_newIntegerFromInt32((int32_t) atoi(valText));
            break;
        case IEC61850_VISIBLE_STRING_32:
        case IEC61850_VISIBLE_STRING_64:
        case IEC61850_VISIBLE_STRING_65:
        case IEC61850_VISIBLE_STRING_129:
        case IEC61850_VISIBLE_STRING_255:
        case IEC61850_UNICODE_STRING_255:
            value = MmsValue_newVisibleString(valText);
            break;
        default:
            /* Composite/exotic types (Struct, Quality, Timestamp, ...) aren't given
             * scalar <Val> overrides in practice in the samples seen - skip rather
             * than guess at an encoding. */
            break;
    }

    if (value) {
        DataAttribute_setValue(da, value); /* clones internally, doesn't take ownership */
        MmsValue_delete(value);
    }
}

static void
applyDoiDaiOverrides(mxml_node_t* lnNode, LogicalNode* ln) {
    for (mxml_node_t* doi = firstElementChild(lnNode); doi; doi = nextElementSibling(doi)) {
        if (!isElement(doi, "DOI")) continue;
        const char* doName = IedModelUtils_attrRequired(doi, "name");
        if (!doName) continue;

        ModelNode* doNode = ModelNode_getChild((ModelNode*) ln, doName);
        if (!doNode) continue;

        for (mxml_node_t* dai = firstElementChild(doi); dai; dai = nextElementSibling(dai)) {
            if (!isElement(dai, "DAI")) continue;
            const char* daName = IedModelUtils_attrRequired(dai, "name");
            if (!daName) continue;

            ModelNode* daNode = ModelNode_getChild(doNode, daName);
            if (!daNode || ModelNode_getType(daNode) != DataAttributeModelType) continue;

            mxml_node_t* valNode = findFirstChildElement(dai, "Val");
            if (!valNode) continue;

            const char* valText = mxmlGetOpaque(valNode);
            if (!valText) continue;

            applyValueOverride((DataAttribute*) daNode, valText);
        }
    }
}

/* ---- LDevice/FCDA reference resolution ----
 *
 * Real-world SCL files are inconsistent about whether FCDA/@ldInst matches the bare
 * LDevice/@inst value (per IEC 61850-6) or an already-concatenated IED+LD wire name
 * (observed directly in libiec61850's own complexModel.icd fixture, which uses
 * ldInst="ied1Inverter" against an LDevice whose actual inst="Inverter" - while
 * sampleModel_with_dataset.icd uses the bare form). Try both rather than guessing
 * which convention a given file follows. */

static char*
buildWireLdName(const char* iedName, const char* ldInst) {
    size_t len = strlen(iedName) + strlen(ldInst) + 1;
    char* name = malloc(len);
    if (name) snprintf(name, len, "%s%s", iedName, ldInst);
    return name;
}

static LogicalDevice*
resolveLogicalDeviceByFcdaRef(IedModel* model, const char* iedName, const char* ldInstRef) {
    LogicalDevice* ld = IedModel_getDeviceByInst(model, ldInstRef);
    if (ld) return ld;

    size_t iedNameLen = strlen(iedName);
    if (strncmp(ldInstRef, iedName, iedNameLen) == 0) {
        return IedModel_getDeviceByInst(model, ldInstRef + iedNameLen);
    }
    return NULL;
}

/* ---- DataSet / FCDA ---- */

static void
buildDataSets(mxml_node_t* lnNode, LogicalNode* ln, LoaderContext* ctx) {
    for (mxml_node_t* dsNode = firstElementChild(lnNode); dsNode; dsNode = nextElementSibling(dsNode)) {
        if (!isElement(dsNode, "DataSet")) continue;
        const char* dsName = IedModelUtils_attrRequired(dsNode, "name");
        if (!dsName) {
            fprintf(stderr, "[ied_model] WARN: skipping DataSet with missing name under LN '%s'\n", ln->name);
            continue;
        }

        DataSet* dataSet = DataSet_create(dsName, ln);

        for (mxml_node_t* fcda = firstElementChild(dsNode); fcda; fcda = nextElementSibling(fcda)) {
            if (!isElement(fcda, "FCDA")) continue;

            const char* ldInstRef = IedModelUtils_attrRequired(fcda, "ldInst");
            const char* lnClass = IedModelUtils_attrOrDefault(fcda, "lnClass", "LLN0");
            const char* lnInst = IedModelUtils_attrOrDefault(fcda, "lnInst", "");
            const char* prefix = IedModelUtils_attrOrDefault(fcda, "prefix", "");
            const char* fc = IedModelUtils_attrRequired(fcda, "fc");
            const char* doName = IedModelUtils_attrRequired(fcda, "doName");
            const char* daName = IedModelUtils_attrOrDefault(fcda, "daName", NULL);

            if (!ldInstRef || !fc || !doName) {
                fprintf(stderr, "[ied_model] WARN: skipping malformed FCDA in DataSet '%s'\n", dsName);
                continue;
            }

            LogicalDevice* targetLd = resolveLogicalDeviceByFcdaRef(ctx->model, ctx->iedName, ldInstRef);
            if (!targetLd) {
                fprintf(stderr, "[ied_model] WARN: FCDA references unresolved LDevice '%s' in DataSet '%s'\n",
                        ldInstRef, dsName);
                continue;
            }

            char* ldWireName = buildWireLdName(ctx->iedName, targetLd->name);
            char* variable = ldWireName
                    ? IedModelUtils_buildFcdaVariableName(ldWireName, lnClass, lnInst, prefix, fc, doName, daName)
                    : NULL;

            if (variable) {
                DataSetEntry_create(dataSet, variable, -1, NULL);
                free(variable);
            }
            free(ldWireName);
        }
    }
}

/* ---- ReportControl ---- */

static void
buildReportControls(mxml_node_t* lnNode, LogicalNode* ln) {
    for (mxml_node_t* rcNode = firstElementChild(lnNode); rcNode; rcNode = nextElementSibling(rcNode)) {
        if (!isElement(rcNode, "ReportControl")) continue;

        const char* name = IedModelUtils_attrRequired(rcNode, "name");
        const char* datSet = IedModelUtils_attrRequired(rcNode, "datSet");
        if (!name || !datSet) {
            fprintf(stderr, "[ied_model] WARN: skipping malformed ReportControl under LN '%s'\n", ln->name);
            continue;
        }

        const char* rptId = IedModelUtils_attrOrDefault(rcNode, "rptID", NULL);
        bool buffered = IedModelUtils_attrBool(rcNode, "buffered", false);
        uint32_t confRev = (uint32_t) IedModelUtils_attrInt(rcNode, "confRev", 1);
        uint32_t bufTime = (uint32_t) IedModelUtils_attrInt(rcNode, "bufTime", 0);
        uint32_t intgPd = (uint32_t) IedModelUtils_attrInt(rcNode, "intgPd", 0);

        mxml_node_t* trgOpsNode = findFirstChildElement(rcNode, "TrgOps");
        mxml_node_t* optFieldsNode = findFirstChildElement(rcNode, "OptFields");

        uint8_t trgOps = IedModelUtils_buildTrgOps(trgOpsNode);
        uint8_t optFlds = IedModelUtils_buildOptFlds(optFieldsNode);

        ReportControlBlock_create(name, ln, rptId, buffered, datSet, confRev, trgOps, optFlds, bufTime, intgPd);
    }
}

/* ---- GSEControl + Communication addressing ---- */

static bool
parseMacAddress(const char* text, uint8_t mac[6]) {
    unsigned int b[6];
    if (sscanf(text, "%2x-%2x-%2x-%2x-%2x-%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t) b[i];
    return true;
}

static mxml_node_t*
findCommunicationGseAddress(mxml_node_t* sclRoot, const char* iedName, const char* apName,
        const char* ldInst, const char* cbName) {
    mxml_node_t* comm = mxmlFindElement(sclRoot, sclRoot, "Communication", NULL, NULL, MXML_DESCEND);
    if (!comm) return NULL;

    /* mxmlFindElement only returns the first match per call, and there can be several
     * ConnectedAP/GSE entries - iterate manually rather than relying on a single call. */
    for (mxml_node_t* sn = firstElementChild(comm); sn; sn = nextElementSibling(sn)) {
        if (!isElement(sn, "SubNetwork")) continue;

        for (mxml_node_t* cap = firstElementChild(sn); cap; cap = nextElementSibling(cap)) {
            if (!isElement(cap, "ConnectedAP")) continue;

            const char* capIedName = IedModelUtils_attrOrDefault(cap, "iedName", "");
            const char* capApName = IedModelUtils_attrOrDefault(cap, "apName", "");
            if (strcmp(capIedName, iedName) != 0 || strcmp(capApName, apName) != 0) continue;

            for (mxml_node_t* gse = firstElementChild(cap); gse; gse = nextElementSibling(gse)) {
                if (!isElement(gse, "GSE")) continue;

                const char* gseLdInst = IedModelUtils_attrOrDefault(gse, "ldInst", "");
                const char* gseCbName = IedModelUtils_attrOrDefault(gse, "cbName", "");
                if (strcmp(gseLdInst, ldInst) == 0 && strcmp(gseCbName, cbName) == 0) {
                    return gse;
                }
            }
        }
    }

    return NULL;
}

static void
attachPhyComAddress(GSEControlBlock* gcb, mxml_node_t* gseNode) {
    mxml_node_t* addr = findFirstChildElement(gseNode, "Address");
    if (!addr) return;

    uint16_t vlanId = 0, appId = 0;
    uint8_t vlanPriority = 0;
    uint8_t dstMac[6] = { 0 };
    bool haveMac = false;

    for (mxml_node_t* p = firstElementChild(addr); p; p = nextElementSibling(p)) {
        if (!isElement(p, "P")) continue;

        const char* type = IedModelUtils_attrOrDefault(p, "type", "");
        const char* text = mxmlGetOpaque(p);
        if (!text) continue;

        if (strcmp(type, "VLAN-ID") == 0) {
            vlanId = (uint16_t) strtoul(text, NULL, 0);
        } else if (strcmp(type, "VLAN-PRIORITY") == 0) {
            vlanPriority = (uint8_t) atoi(text);
        } else if (strcmp(type, "APPID") == 0) {
            appId = (uint16_t) strtoul(text, NULL, 0);
        } else if (strcmp(type, "MAC-Address") == 0) {
            haveMac = parseMacAddress(text, dstMac);
        }
    }

    if (haveMac) {
        PhyComAddress* phyAddr = PhyComAddress_create(vlanPriority, vlanId, appId, dstMac);
        GSEControlBlock_addPhyComAddress(gcb, phyAddr);
    }
}

static void
buildGseControls(mxml_node_t* lnNode, LogicalNode* ln, LoaderContext* ctx, const char* apName, const char* ldInst) {
    for (mxml_node_t* gseNode = firstElementChild(lnNode); gseNode; gseNode = nextElementSibling(gseNode)) {
        if (!isElement(gseNode, "GSEControl")) continue;

        const char* name = IedModelUtils_attrRequired(gseNode, "name");
        const char* datSet = IedModelUtils_attrRequired(gseNode, "datSet");
        if (!name || !datSet) {
            fprintf(stderr, "[ied_model] WARN: skipping malformed GSEControl under LN '%s'\n", ln->name);
            continue;
        }

        const char* appId = IedModelUtils_attrOrDefault(gseNode, "appID", "");
        uint32_t confRev = (uint32_t) IedModelUtils_attrInt(gseNode, "confRev", 1);

        GSEControlBlock* gcb = GSEControlBlock_create(name, ln, appId, datSet, confRev, false, -1, -1);

        mxml_node_t* gseAddr = findCommunicationGseAddress(ctx->sclRoot, ctx->iedName, apName, ldInst, name);
        if (gseAddr) {
            attachPhyComAddress(gcb, gseAddr);
        }
        /* else: no matching <Communication> entry - address left unset. Not an error;
         * typical for plain .icd files with no real network config assigned yet. */
    }
}

/* ---- LogicalNode / LDevice / AccessPoint traversal ----
 *
 * Two passes over the same tree, deliberately: DataSet/FCDA entries and
 * GSEControl/ReportControl can reference *any* LDevice within the IED,
 * including ones declared later in the file (confirmed directly against
 * complexModel.icd, whose single <DataSet> under LDevice "Inverter"
 * references LDevice "Battery", declared afterwards). Building structure
 * and cross-references in one interleaved top-to-bottom walk means a
 * forward reference resolves against a model that doesn't have the target
 * LDevice yet. Pass 1 builds every LDevice/LN/DO/DA first; pass 2 (which
 * can safely resolve any LDevice in the IED) builds DataSets/ReportControl/
 * GSEControl.
 */

static char*
lnInstanceName(mxml_node_t* lnNode) {
    const char* lnClass = IedModelUtils_attrRequired(lnNode, "lnClass");
    const char* inst = IedModelUtils_attrOrDefault(lnNode, "inst", "");
    const char* prefix = IedModelUtils_attrOrDefault(lnNode, "prefix", "");
    if (!lnClass) return NULL;
    return IedModelUtils_buildLnName(prefix, lnClass, inst);
}

static void
buildLogicalNodeStructure(mxml_node_t* lnNode, LogicalDevice* ld, LoaderContext* ctx, const char* ldInst) {
    const char* lnType = IedModelUtils_attrRequired(lnNode, "lnType");
    char* lnName = lnInstanceName(lnNode);

    if (!lnName || !lnType) {
        fprintf(stderr, "[ied_model] WARN: skipping malformed LN/LN0 under LDevice '%s' (missing lnClass/lnType)\n",
                ldInst);
        free(lnName);
        return;
    }

    LogicalNode* ln = LogicalNode_create(lnName, ld);

    mxml_node_t* lNodeType = findTemplateById(ctx->templates, "LNodeType", lnType);
    if (!lNodeType) {
        fprintf(stderr, "[ied_model] WARN: unresolved LNodeType '%s' for LN '%s'\n", lnType, lnName);
    } else {
        for (mxml_node_t* doNode = firstElementChild(lNodeType); doNode; doNode = nextElementSibling(doNode)) {
            if (!isElement(doNode, "DO")) continue;

            const char* doName = IedModelUtils_attrRequired(doNode, "name");
            const char* doTypeId = IedModelUtils_attrRequired(doNode, "type");
            if (!doName || !doTypeId) {
                fprintf(stderr, "[ied_model] WARN: skipping malformed DO under LNodeType '%s'\n", lnType);
                continue;
            }
            buildDataObject(doName, doTypeId, (ModelNode*) ln, ctx->templates);
        }
    }

    applyDoiDaiOverrides(lnNode, ln);

    free(lnName);
}

static void
buildLogicalNodeReferences(mxml_node_t* lnNode, LogicalDevice* ld, LoaderContext* ctx, const char* apName,
        const char* ldInst) {
    char* lnName = lnInstanceName(lnNode);
    if (!lnName) return;

    LogicalNode* ln = LogicalDevice_getLogicalNode(ld, lnName);
    if (!ln) {
        /* Structure pass already warned about this LN; nothing to attach references to. */
        free(lnName);
        return;
    }

    buildDataSets(lnNode, ln, ctx);
    buildReportControls(lnNode, ln);
    buildGseControls(lnNode, ln, ctx, apName, ldInst);

    free(lnName);
}

static void
buildLogicalDeviceStructure(mxml_node_t* ldNode, LoaderContext* ctx) {
    const char* ldInst = IedModelUtils_attrRequired(ldNode, "inst");
    if (!ldInst) {
        fprintf(stderr, "[ied_model] WARN: skipping LDevice with missing inst attribute\n");
        return;
    }

    LogicalDevice* ld = LogicalDevice_create(ldInst, ctx->model);

    for (mxml_node_t* lnNode = firstElementChild(ldNode); lnNode; lnNode = nextElementSibling(lnNode)) {
        if (!isElement(lnNode, "LN0") && !isElement(lnNode, "LN")) continue;
        buildLogicalNodeStructure(lnNode, ld, ctx, ldInst);
    }
}

static void
buildLogicalDeviceReferences(mxml_node_t* ldNode, LoaderContext* ctx, const char* apName) {
    const char* ldInst = IedModelUtils_attrRequired(ldNode, "inst");
    if (!ldInst) return;

    LogicalDevice* ld = IedModel_getDeviceByInst(ctx->model, ldInst);
    if (!ld) return; /* structure pass already warned */

    for (mxml_node_t* lnNode = firstElementChild(ldNode); lnNode; lnNode = nextElementSibling(lnNode)) {
        if (!isElement(lnNode, "LN0") && !isElement(lnNode, "LN")) continue;
        buildLogicalNodeReferences(lnNode, ld, ctx, apName, ldInst);
    }
}

static void
buildAccessPointStructure(mxml_node_t* apNode, LoaderContext* ctx) {
    mxml_node_t* server = findFirstChildElement(apNode, "Server");
    if (!server) return;

    for (mxml_node_t* ldNode = firstElementChild(server); ldNode; ldNode = nextElementSibling(ldNode)) {
        if (!isElement(ldNode, "LDevice")) continue;
        buildLogicalDeviceStructure(ldNode, ctx);
    }
}

static void
buildAccessPointReferences(mxml_node_t* apNode, LoaderContext* ctx) {
    const char* apName = IedModelUtils_attrOrDefault(apNode, "name", "");
    mxml_node_t* server = findFirstChildElement(apNode, "Server");
    if (!server) return;

    for (mxml_node_t* ldNode = firstElementChild(server); ldNode; ldNode = nextElementSibling(ldNode)) {
        if (!isElement(ldNode, "LDevice")) continue;
        buildLogicalDeviceReferences(ldNode, ctx, apName);
    }
}

/* ---- entry point ---- */

IedModel*
IedModelSclLoader_load(const char* path, const char* iedName, IedModelLoadError* outError) {
    *outError = IED_MODEL_OK;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        *outError = IED_MODEL_ERR_FILE_NOT_FOUND;
        return NULL;
    }

    mxml_node_t* tree = mxmlLoadFile(NULL, fp, MXML_OPAQUE_CALLBACK);
    fclose(fp);

    if (!tree) {
        *outError = IED_MODEL_ERR_XML_PARSE;
        return NULL;
    }

    mxml_node_t* sclRoot = firstElementChild(tree);
    if (!sclRoot) {
        *outError = IED_MODEL_ERR_XML_PARSE;
        mxmlDelete(tree);
        return NULL;
    }

    mxml_node_t* iedNode = mxmlFindElement(sclRoot, sclRoot, "IED", "name", iedName, MXML_DESCEND);
    if (!iedNode) {
        *outError = IED_MODEL_ERR_IED_NOT_FOUND;
        mxmlDelete(tree);
        return NULL;
    }

    /* May legitimately be NULL - individual type lookups then fail gracefully (warn +
     * skip that node) rather than crash. */
    mxml_node_t* templates = mxmlFindElement(sclRoot, sclRoot, "DataTypeTemplates", NULL, NULL, MXML_DESCEND);

    IedModel* model = IedModel_create(iedName);
    if (!model) {
        *outError = IED_MODEL_ERR_OUT_OF_MEMORY;
        mxmlDelete(tree);
        return NULL;
    }

    LoaderContext ctx = { .model = model, .sclRoot = sclRoot, .templates = templates, .iedName = iedName };

    for (mxml_node_t* apNode = firstElementChild(iedNode); apNode; apNode = nextElementSibling(apNode)) {
        if (!isElement(apNode, "AccessPoint")) continue;
        buildAccessPointStructure(apNode, &ctx);
    }

    for (mxml_node_t* apNode = firstElementChild(iedNode); apNode; apNode = nextElementSibling(apNode)) {
        if (!isElement(apNode, "AccessPoint")) continue;
        buildAccessPointReferences(apNode, &ctx);
    }

    mxmlDelete(tree);
    return model;
}
