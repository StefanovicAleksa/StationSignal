#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model/data/ied_model_scl_loader.h"
#include "features/ied_model/utils/ied_model_utils.h"
#include "features/ied_model/utils/ied_model_ln_category.h"
#include "iec61850_dynamic_model.h"
#include "mms_value.h"
#include "mxml.h"

typedef struct {
    IedModel* model;
    mxml_node_t* sclRoot;
    mxml_node_t* templates; /* may be NULL - every lookup against it must handle that */
    const char* iedName;
    LinkedList daSemantics;   /* model-scoped (unlike enumAttrs, which is LN-scoped and
                                  created/destroyed per-LN) - see buildDataAttribute */
    LinkedList lnCategories;  /* model-scoped, one IedModelLnCategoryEntry* per LN - see
                                  buildLogicalNodeStructure */
    LinkedList daDescriptions; /* model-scoped, one IedModelDaDescEntry* per DA with a desc -
                                   see buildDataAttribute (template level) and
                                   applyOverridesUnderDataObject (DAI instance level, wins) */
} LoaderContext;

/*
 * One per Enum-typed DataAttribute built anywhere under one LN, recording
 * which <EnumType> template it was declared against - captured during the
 * structure pass (buildDataAttribute) so the override pass
 * (applyValueOverride) can resolve a <DAI>'s Val label to its real ordinal
 * instead of guessing. enumTypeId points into the live mxml tree - valid
 * for the whole load, not owned by this entry.
 */
typedef struct {
    ModelNode* da;
    const char* enumTypeId;
} EnumAttrEntry;

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

static void buildDataObject(const char* name, const char* doTypeId, ModelNode* parent, mxml_node_t* templates,
        LinkedList enumAttrs, LinkedList daSemantics, LinkedList daDescriptions);

/* Appends {da, strdup(desc)} to daDescriptions - used both for a DA/BDA
 * template's own desc (buildDataAttribute) and, later, a <DAI> instance
 * override (applyOverridesUnderDataObject) which must instead REPLACE
 * whatever template-level entry already exists for the same `da` (instance
 * wins over template - same precedence Val overrides already use). Two
 * separate helpers, not one shared upsert, since the template pass never
 * needs to check for an existing entry (each DataAttribute is only built
 * once) while the override pass always does. */
static void
appendDaDescription(LinkedList daDescriptions, DataAttribute* da, const char* desc) {
    if (!daDescriptions || !desc) return;
    IedModelDaDescEntry* entry = malloc(sizeof(IedModelDaDescEntry));
    if (!entry) return;
    entry->da = da;
    entry->desc = strdup(desc);
    if (!entry->desc) {
        free(entry);
        return;
    }
    LinkedList_add(daDescriptions, entry);
}

/* LinkedListValueDeleteFunction-compatible: frees a boxed IedModelDaDescEntry
 * AND its owned `desc` string - plain `free` (used for daSemantics/
 * lnCategories entries, which own nothing beyond the box itself) would leak
 * `desc` here. */
static void
freeDaDescEntry(void* entry) {
    if (!entry) return;
    IedModelDaDescEntry* descEntry = (IedModelDaDescEntry*) entry;
    free(descEntry->desc);
    free(descEntry);
}

static void
buildDataAttribute(mxml_node_t* node, ModelNode* parent, const char* inheritedFc, mxml_node_t* templates,
        LinkedList enumAttrs, LinkedList daSemantics, LinkedList daDescriptions) {
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

    if (strcmp(bType, "Dbpos") == 0 && daSemantics) {
        /* Captured from the raw SCL bType string directly, BEFORE
         * IedModelUtils_mapBType collapses it into the generic
         * IEC61850_CODEDENUM type alongside "Tcmd" - see
         * IedModelDaSemantic's own doc comment for why this can't be
         * recovered later from `type` alone. */
        IedModelDaSemanticEntry* entry = malloc(sizeof(IedModelDaSemanticEntry));
        if (entry) {
            entry->da = da;
            entry->semantic = IED_MODEL_DA_SEMANTIC_DBPOS;
            LinkedList_add(daSemantics, entry);
        }
    }

    /* Template-level desc="..." - may be overwritten later by a <DAI>
     * instance-level desc for this same `da` (applyOverridesUnderDataObject),
     * which takes precedence, same as Val overrides already do. */
    appendDaDescription(daDescriptions, da, IedModelUtils_attrOrDefault(node, "desc", NULL));

    if (type == IEC61850_ENUMERATED && enumAttrs) {
        /* type= here is the EnumType/@id (schema overloads the same "type"
         * attribute DAType/DOType references use) - captured now so a later
         * <DAI> override can resolve its Val label against the real
         * EnumType, see EnumAttrEntry's own doc comment. */
        const char* enumTypeId = IedModelUtils_attrOrDefault(node, "type", NULL);
        if (enumTypeId) {
            EnumAttrEntry* entry = malloc(sizeof(EnumAttrEntry));
            if (entry) {
                entry->da = (ModelNode*) da;
                entry->enumTypeId = enumTypeId;
                LinkedList_add(enumAttrs, entry);
            }
        }
    }

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
                buildDataAttribute(bda, (ModelNode*) da, fcStr, templates, enumAttrs, daSemantics, daDescriptions);
            }
        }
    }
}

static void
buildDataObject(const char* name, const char* doTypeId, ModelNode* parent, mxml_node_t* templates,
        LinkedList enumAttrs, LinkedList daSemantics, LinkedList daDescriptions) {
    DataObject* dobj = DataObject_create(name, parent, 0);

    mxml_node_t* doType = findTemplateById(templates, "DOType", doTypeId);
    if (!doType) {
        fprintf(stderr, "[ied_model] WARN: unresolved DOType '%s' for data object '%s'\n",
                doTypeId ? doTypeId : "(missing)", name);
        return;
    }

    for (mxml_node_t* child = firstElementChild(doType); child; child = nextElementSibling(child)) {
        if (isElement(child, "DA")) {
            buildDataAttribute(child, (ModelNode*) dobj, NULL, templates, enumAttrs, daSemantics, daDescriptions);
        } else if (isElement(child, "SDO")) {
            const char* sdoName = IedModelUtils_attrRequired(child, "name");
            const char* sdoTypeId = IedModelUtils_attrRequired(child, "type");
            if (!sdoName || !sdoTypeId) {
                fprintf(stderr, "[ied_model] WARN: skipping malformed SDO under DOType '%s'\n", doTypeId);
                continue;
            }
            buildDataObject(sdoName, sdoTypeId, (ModelNode*) dobj, templates, enumAttrs, daSemantics, daDescriptions);
        }
    }
}

/* ---- DOI/DAI/Val default-value overrides ---- */

/*
 * Resolves an enum <DAI>'s Val label (e.g. "status-only") to its real
 * ordinal via the DA's own EnumType (looked up through enumAttrs, captured
 * during the structure pass - see EnumAttrEntry). Falls back to accepting a
 * raw numeric ordinal directly (legal, just uncommon) - requiring the
 * ENTIRE string to be numeric so a non-numeric label never silently
 * resolves to ordinal 0 the way a bare atoi() would. Returns false (leave
 * the attribute at its default, caller warns) if neither resolves.
 */
static bool
lookupEnumOrdinal(mxml_node_t* templates, LinkedList enumAttrs, DataAttribute* da, const char* valText,
        int32_t* outOrdinal) {
    const char* enumTypeId = NULL;
    if (enumAttrs) {
        for (LinkedList element = LinkedList_getNext(enumAttrs); element; element = LinkedList_getNext(element)) {
            EnumAttrEntry* entry = (EnumAttrEntry*) LinkedList_getData(element);
            if (entry->da == (ModelNode*) da) {
                enumTypeId = entry->enumTypeId;
                break;
            }
        }
    }

    if (enumTypeId) {
        mxml_node_t* enumType = findTemplateById(templates, "EnumType", enumTypeId);
        if (enumType) {
            for (mxml_node_t* ev = firstElementChild(enumType); ev; ev = nextElementSibling(ev)) {
                if (!isElement(ev, "EnumVal")) continue;
                const char* label = mxmlGetOpaque(ev);
                if (!label || strcmp(label, valText) != 0) continue;

                const char* ord = IedModelUtils_attrOrDefault(ev, "ord", NULL);
                if (ord) {
                    *outOrdinal = atoi(ord);
                    return true;
                }
            }
        }
    }

    char* end = NULL;
    long parsed = strtol(valText, &end, 10);
    if (end != valText && *end == '\0') {
        *outOrdinal = (int32_t) parsed;
        return true;
    }

    return false;
}

static void
applyValueOverride(DataAttribute* da, const char* valText, mxml_node_t* templates, LinkedList enumAttrs) {
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
        case IEC61850_ENUMERATED: {
            int32_t ordinal;
            if (lookupEnumOrdinal(templates, enumAttrs, da, valText, &ordinal)) {
                value = MmsValue_newIntegerFromInt32(ordinal);
            } else {
                fprintf(stderr,
                        "[ied_model] WARN: enum value '%s' not found in EnumType for attribute '%s' - "
                        "override skipped (left at default, not guessed)\n", valText, da->name);
            }
            break;
        }
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

/* Replaces the desc for `da` in daDescriptions if an entry already exists
 * (the template-level one from buildDataAttribute), else appends a new one.
 * Instance-level (<DAI>) desc always wins over template-level - same
 * precedence Val overrides already use in applyValueOverride. */
static void
upsertDaDescription(LinkedList daDescriptions, DataAttribute* da, const char* desc) {
    if (!daDescriptions || !desc) return;
    for (LinkedList element = LinkedList_getNext(daDescriptions); element; element = LinkedList_getNext(element)) {
        IedModelDaDescEntry* entry = (IedModelDaDescEntry*) LinkedList_getData(element);
        if (entry->da == da) {
            char* replacement = strdup(desc);
            if (replacement) {
                free(entry->desc);
                entry->desc = replacement;
            }
            return;
        }
    }
    appendDaDescription(daDescriptions, da, desc);
}

/*
 * Recursively applies <DAI>/<SDI> value overrides under one DO (or nested
 * structured child), mirroring buildDataObject's own SDO recursion shape.
 * <SDI> wraps a structured/array DO's nested child overrides and can nest
 * arbitrarily deep (confirmed common in real files - tens of thousands of
 * occurrences across this repo's own sample SCDs) - previously silently
 * dropped entirely since only direct <DAI> children were ever visited.
 */
static void
applyOverridesUnderDataObject(mxml_node_t* container, ModelNode* doNode, mxml_node_t* templates,
        LinkedList enumAttrs, LinkedList daDescriptions) {
    for (mxml_node_t* child = firstElementChild(container); child; child = nextElementSibling(child)) {
        if (isElement(child, "DAI")) {
            const char* daName = IedModelUtils_attrRequired(child, "name");
            if (!daName) {
                fprintf(stderr, "[ied_model] WARN: skipping DAI with missing name under '%s'\n", doNode->name);
                continue;
            }

            ModelNode* daNode = ModelNode_getChild(doNode, daName);
            if (!daNode || ModelNode_getType(daNode) != DataAttributeModelType) {
                fprintf(stderr,
                        "[ied_model] WARN: DAI '%s' has no matching DataAttribute under '%s' - override dropped\n",
                        daName, doNode->name);
                continue;
            }

            /* Instance-level desc, independent of whether this DAI also carries
             * a Val override below - a DAI can exist purely to attach a
             * human-readable label with no value override at all. */
            upsertDaDescription(daDescriptions, (DataAttribute*) daNode,
                    IedModelUtils_attrOrDefault(child, "desc", NULL));

            mxml_node_t* valNode = findFirstChildElement(child, "Val");
            if (!valNode) continue; /* legal: DAI with no Val (e.g. just sAddr/ix) - not an error */

            const char* valText = mxmlGetOpaque(valNode);
            if (!valText) continue;

            applyValueOverride((DataAttribute*) daNode, valText, templates, enumAttrs);
        } else if (isElement(child, "SDI")) {
            const char* sdiName = IedModelUtils_attrRequired(child, "name");
            if (!sdiName) {
                fprintf(stderr, "[ied_model] WARN: skipping SDI with missing name under '%s'\n", doNode->name);
                continue;
            }

            ModelNode* nested = ModelNode_getChild(doNode, sdiName);
            if (!nested) {
                fprintf(stderr,
                        "[ied_model] WARN: SDI '%s' has no matching structured child under '%s' - override(s) dropped\n",
                        sdiName, doNode->name);
                continue;
            }

            applyOverridesUnderDataObject(child, nested, templates, enumAttrs, daDescriptions);
        }
    }
}

/* Note: <DOI desc="..."> itself (a DO-level description, sibling to whatever
 * DAI/SDI children it has) is deliberately NOT captured anywhere in this
 * pass - it describes the whole DataObject, not any single terminal
 * DataAttribute, and IedModelDaDescEntry is keyed by DataAttribute* with no
 * single principled leaf to attribute it to (which child DA is "the" value
 * varies by CDC). Only DA/BDA template-level and DAI instance-level desc are
 * captured, since both map onto exactly one DataAttribute each. */
static void
applyDoiDaiOverrides(mxml_node_t* lnNode, LogicalNode* ln, mxml_node_t* templates, LinkedList enumAttrs,
        LinkedList daDescriptions) {
    for (mxml_node_t* doi = firstElementChild(lnNode); doi; doi = nextElementSibling(doi)) {
        if (!isElement(doi, "DOI")) continue;
        const char* doName = IedModelUtils_attrRequired(doi, "name");
        if (!doName) continue;

        ModelNode* doNode = ModelNode_getChild((ModelNode*) ln, doName);
        if (!doNode) {
            fprintf(stderr, "[ied_model] WARN: DOI '%s' has no matching DataObject under LN '%s'\n", doName,
                    ln->name);
            continue;
        }

        applyOverridesUnderDataObject(doi, doNode, templates, enumAttrs, daDescriptions);
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
        ld = IedModel_getDeviceByInst(model, ldInstRef + iedNameLen);
        if (ld) return ld;
    }

    /* Third convention: FCDA/@ldInst matches an LDevice's functional-naming
     * ldName (SCL "functional naming" convention) rather than its bare
     * @inst or the IED-prefixed wire form above. IedModel_getDevice looks
     * up by MMS domain name, which is exactly ldName when functional naming
     * is in effect (see LogicalDevice_createEx's own doc). Not observed in
     * this repo's own sample files, but same bug class as the RCB-name fix
     * (SCL-declared name diverging from the true runtime/reference name),
     * and cheap to close now that ldName is threaded through (see
     * buildLogicalDeviceStructure). */
    return IedModel_getDevice(model, ldInstRef);
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

/*
 * Some real devices (confirmed against a real Siemens SIPROTEC relay's own
 * predefined report control blocks) append a numeric instance suffix (e.g.
 * "01") to a ReportControl's actual runtime object name that is reflected
 * in its rptID attribute but NOT in its own "name" attribute - e.g.
 * name="brcbA" while the live device's real RCB is "brcbA01".
 * IedConnection_getRCBValues fails with IED_ERROR_OBJECT_DOES_NOT_EXIST
 * against the bare SCL name in that case, even though every other part of
 * the model (IED/LDevice/LN names) matches the live device exactly.
 *
 * rptID's own reference convention is "<LDName>/<LN>$<FC>$<rcbName>" - the
 * same "$"-joined convention used elsewhere in this codebase (e.g.
 * ipc_dispatcher's quality-pairing splits on the last "$" too). When rptID
 * has that shape, the substring after the last "$" is the real runtime
 * name; otherwise (no rptID, or one with no "$", as in every one of this
 * repo's own test fixtures) the plain SCL name attribute is already correct.
 *
 * Separately (confirmed against a real ABB REC650 via a live directory-dump
 * diagnostic and reproduced end-to-end with this repo's own simulator, see
 * integration_tests/orchestration_local_file/): a <ReportControl> with a
 * child <RptEnabled max="N"> (N > 1) is pre-instantiated N times by the
 * server, one per potential simultaneous client, and each instance is only
 * addressable on the wire with a zero-padded 2-digit suffix appended to the
 * name - e.g. name="rcb_A" with max="5" is only ever "rcb_A01".."rcb_A05" on
 * the wire, never bare "rcb_A". This daemon is always exactly one client, so
 * it always targets instance 1 ("01") - it never tries other instances if
 * that one turns out to already be claimed by another client, since nothing
 * so far has shown that's a real scenario worth handling.
 *
 * Returns a newly heap-allocated string in every case (caller must free) -
 * unlike its previous borrowed-pointer-only form, a maxInstances>1 suffix
 * requires an allocation, so the no-suffix path was made to allocate too
 * rather than have two different ownership rules depending on input.
 */
static char*
resolveRcbRuntimeName(const char* sclName, const char* rptId, uint32_t maxInstances) {
    const char* base = sclName;
    if (rptId) {
        const char* lastDollar = strrchr(rptId, '$');
        if (lastDollar && lastDollar[1]) base = lastDollar + 1;
    }

    if (maxInstances <= 1) {
        size_t len = strlen(base) + 1;
        char* name = malloc(len);
        if (name) snprintf(name, len, "%s", base);
        return name;
    }

    size_t len = strlen(base) + 2 /* 2-digit instance suffix */ + 1;
    char* name = malloc(len);
    if (name) snprintf(name, len, "%s%02u", base, 1u);
    return name;
}

/*
 * True for a vendor pattern (confirmed against a real Siemens SIPROTEC
 * export) where a control block isn't a real SCL element at all - it's
 * HTML-escaped text embedded inside a <Private type="Siemens-ControlBlockStorage">
 * wrapper element, meant to be "activated" into a literal element only once
 * assigned in a project. `strstr` (substring, not exact match) deliberately,
 * to also catch other vendor-prefixed "...ControlBlockStorage..." Private
 * types beyond the one confirmed exact string. Shared by
 * containsPrivateControlBlockStorage (diagnostic scan) and
 * buildReportControls (actual parsing, see processControlBlockStoragePrivate).
 */
static bool
isControlBlockStoragePrivate(mxml_node_t* privateNode) {
    const char* type = IedModelUtils_attrOrDefault(privateNode, "type", "");
    return strstr(type, "ControlBlockStorage") != NULL;
}

static void
processReportControlNode(mxml_node_t* rcNode, LogicalNode* ln) {
    const char* name = IedModelUtils_attrRequired(rcNode, "name");
    if (!name) {
        fprintf(stderr, "[ied_model] WARN: skipping malformed ReportControl under LN '%s'\n", ln->name);
        return;
    }

    /* datSet is genuinely optional per the SCL schema - a real device's
     * report control block can rely on a server-side default dataset
     * instead of one fixed in SCL (confirmed against a real Siemens
     * SIPROTEC device's own predefined report control blocks, which
     * omit datSet entirely). mms_report_client already handles a NULL
     * datasetReference gracefully - it just skips asserting
     * RCB_ELEMENT_DATSET on enable rather than fabricating one, see that
     * function's own comment. */
    const char* datSet = IedModelUtils_attrOrDefault(rcNode, "datSet", NULL);
    const char* rptId = IedModelUtils_attrOrDefault(rcNode, "rptID", NULL);
    bool buffered = IedModelUtils_attrBool(rcNode, "buffered", false);
    uint32_t confRev = (uint32_t) IedModelUtils_attrInt(rcNode, "confRev", 1);
    uint32_t bufTime = (uint32_t) IedModelUtils_attrInt(rcNode, "bufTime", 0);
    uint32_t intgPd = (uint32_t) IedModelUtils_attrInt(rcNode, "intgPd", 0);

    mxml_node_t* trgOpsNode = findFirstChildElement(rcNode, "TrgOps");
    mxml_node_t* optFieldsNode = findFirstChildElement(rcNode, "OptFields");
    mxml_node_t* rptEnabledNode = findFirstChildElement(rcNode, "RptEnabled");

    uint8_t trgOps = IedModelUtils_buildTrgOps(trgOpsNode);
    uint8_t optFlds = IedModelUtils_buildOptFlds(optFieldsNode);
    uint32_t maxInstances = rptEnabledNode ? (uint32_t) IedModelUtils_attrInt(rptEnabledNode, "max", 1) : 1;

    char* runtimeName = resolveRcbRuntimeName(name, rptId, maxInstances);
    if (runtimeName) {
        ReportControlBlock_create(runtimeName, ln, rptId, buffered, datSet, confRev, trgOps, optFlds, bufTime,
                intgPd);
        free(runtimeName);
    }
}

/*
 * Unescapes and parses one Siemens-style <Private type="...ControlBlockStorage...">
 * payload (see isControlBlockStoragePrivate's own doc comment for the vendor
 * pattern this addresses), feeding a resulting <ReportControl> fragment
 * through the exact same processReportControlNode path a literal SCL element
 * would take - no divergent behavior between "real SCL" and "unescaped from
 * Private storage" RCBs.
 *
 * mxmlGetOpaque() on privateNode already returns fully entity-decoded text
 * (mxml decodes &lt;/&gt;/etc. during parsing itself, independent of the
 * value-type callback used - MXML_OPAQUE_CALLBACK only controls how the
 * decoded string is subsequently typed/stored) - so no manual HTML-unescaping
 * is needed here, only splitting off the vendor's own leading "<key>|" prefix
 * (a vendor-internal index, mirroring the RCB's own "name" attribute in every
 * sample seen - not itself needed, since the inner element carries every
 * attribute processReportControlNode reads).
 *
 * Only <ReportControl> payloads are unescaped and used - <DataSet>/<GSEControl>
 * wrapped the same way have never been observed in a real export (this
 * station's own SCD wraps 2736/2736 escaped entries as <ReportControl>, zero
 * as anything else) and are deliberately left unhandled (WARN + skip) rather
 * than guessed at.
 */
static void
processControlBlockStoragePrivate(mxml_node_t* privateNode, LogicalNode* ln) {
    const char* payload = mxmlGetOpaque(privateNode);
    if (!payload || !payload[0]) {
        fprintf(stderr, "[ied_model] WARN: empty Private ControlBlockStorage payload under LN '%s'\n", ln->name);
        return;
    }

    const char* separator = strchr(payload, '|');
    if (!separator || !separator[1]) {
        fprintf(stderr, "[ied_model] WARN: malformed Private ControlBlockStorage payload under LN '%s' "
                "(no '<key>|<xml>' separator)\n", ln->name);
        return;
    }

    mxml_node_t* fragmentRoot = mxmlLoadString(NULL, separator + 1, MXML_OPAQUE_CALLBACK);
    if (!fragmentRoot) {
        fprintf(stderr, "[ied_model] WARN: unparseable Private ControlBlockStorage XML under LN '%s'\n", ln->name);
        return;
    }

    if (isElement(fragmentRoot, "ReportControl")) {
        const char* rcName = IedModelUtils_attrOrDefault(fragmentRoot, "name", "?");
        fprintf(stderr, "[ied_model] parsed ReportControl '%s' from Private ControlBlockStorage under LN '%s'\n",
                rcName, ln->name);
        processReportControlNode(fragmentRoot, ln);
    } else {
        fprintf(stderr, "[ied_model] WARN: Private ControlBlockStorage under LN '%s' wraps an unhandled "
                "element (only ReportControl is parsed today)\n", ln->name);
    }

    mxmlDelete(fragmentRoot);
}

static void
buildReportControls(mxml_node_t* lnNode, LogicalNode* ln) {
    for (mxml_node_t* rcNode = firstElementChild(lnNode); rcNode; rcNode = nextElementSibling(rcNode)) {
        if (isElement(rcNode, "ReportControl")) {
            processReportControlNode(rcNode, ln);
        } else if (isElement(rcNode, "Private") && isControlBlockStoragePrivate(rcNode)) {
            processControlBlockStoragePrivate(rcNode, ln);
        }
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

        /* VLAN-ID/APPID are hex strings per IEC 61850-8-1, never decimal -
         * base-0 autodetection (strtoul's old ", NULL, 0)" form) would treat
         * a leading-zero, no-"0x"-prefixed string as OCTAL instead, silently
         * truncating/mis-parsing any real value containing an '8'/'9' or a
         * hex letter (confirmed against real APPID values like "000A" in
         * this repo's own sample SCDs - previously silently became 0). */
        if (strcmp(type, "VLAN-ID") == 0) {
            vlanId = (uint16_t) strtoul(text, NULL, 16);
        } else if (strcmp(type, "VLAN-PRIORITY") == 0) {
            vlanPriority = (uint8_t) atoi(text);
        } else if (strcmp(type, "APPID") == 0) {
            appId = (uint16_t) strtoul(text, NULL, 16);
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
        /* Unlike ReportControl's datSet (made optional above), GSEControl's
         * datSet is deliberately kept required: reconsidered for symmetry
         * with that fix, but every real GSEControl sample in this repo's
         * own SCDs (86/86 across both files) populates it, so loosening
         * this would be speculative rather than evidence-driven. */
        const char* datSet = IedModelUtils_attrRequired(gseNode, "datSet");
        if (!name || !datSet) {
            fprintf(stderr, "[ied_model] WARN: skipping malformed GSEControl under LN '%s'\n", ln->name);
            continue;
        }

        const char* appId = IedModelUtils_attrOrDefault(gseNode, "appID", "");
        uint32_t confRev = (uint32_t) IedModelUtils_attrInt(gseNode, "confRev", 1);

        /* MinTime/MaxTime live under the same <GSE> as <Address>, in
         * <Communication>, not on <GSEControl> itself - so this lookup must
         * happen before GSEControlBlock_create, since minTime/maxTime are
         * constructor-only params (no post-create setter exists). Previously
         * this lookup ran after creation purely to attach the address,
         * always passing -1,-1 (library-default retransmission timing)
         * regardless of what the file actually specified - confirmed real
         * files populate these (e.g. <MinTime>10</MinTime><MaxTime>5000</MaxTime>). */
        mxml_node_t* gseAddr = findCommunicationGseAddress(ctx->sclRoot, ctx->iedName, apName, ldInst, name);

        int minTime = -1, maxTime = -1;
        if (gseAddr) {
            mxml_node_t* minNode = findFirstChildElement(gseAddr, "MinTime");
            mxml_node_t* maxNode = findFirstChildElement(gseAddr, "MaxTime");
            if (minNode && mxmlGetOpaque(minNode)) minTime = atoi(mxmlGetOpaque(minNode));
            if (maxNode && mxmlGetOpaque(maxNode)) maxTime = atoi(mxmlGetOpaque(maxNode));
        }

        GSEControlBlock* gcb = GSEControlBlock_create(name, ln, appId, datSet, confRev, false, minTime, maxTime);

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
    LinkedList enumAttrs = LinkedList_create();

    /* lnClass is read again here (lnInstanceName already read it once, but
     * discarded it after concatenating lnName) purely to classify the LN by
     * its own SCL-declared class - cheap (one more mxmlElementGetAttr) and
     * keeps lnInstanceName's own signature/contract unchanged. */
    if (ctx->lnCategories) {
        const char* lnClass = IedModelUtils_attrRequired(lnNode, "lnClass");
        IedModelLnCategoryEntry* entry = malloc(sizeof(IedModelLnCategoryEntry));
        if (entry) {
            entry->ln = ln;
            entry->category = IedModelLnCategory_forLnClass(lnClass);
            LinkedList_add(ctx->lnCategories, entry);
        }
    }

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
            buildDataObject(doName, doTypeId, (ModelNode*) ln, ctx->templates, enumAttrs, ctx->daSemantics,
                    ctx->daDescriptions);
        }
    }

    applyDoiDaiOverrides(lnNode, ln, ctx->templates, enumAttrs, ctx->daDescriptions);
    LinkedList_destroyDeep(enumAttrs, free);

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

    /* ldName (SCL "functional naming" convention) is genuinely optional -
     * NULL here reproduces LogicalDevice_create's exact old behavior. When
     * present, it's the true MMS/protocol-side domain name for this LD (see
     * LogicalDevice_createEx's own doc), which resolveLogicalDeviceByFcdaRef's
     * third fallback relies on. Not observed in this repo's own sample
     * files, but same bug class as the RCB-name fix - closing it now that
     * the field is cheap to thread through. */
    const char* ldName = IedModelUtils_attrOrDefault(ldNode, "ldName", NULL);
    LogicalDevice* ld = LogicalDevice_createEx(ldInst, ctx->model, ldName);

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

/*
 * Detects the same vendor pattern isControlBlockStoragePrivate matches,
 * anywhere in the tree, purely to log a diagnostic - `<ReportControl>`
 * payloads found this way are now actually unescaped and parsed (see
 * buildReportControls/processControlBlockStoragePrivate), so this no longer
 * means "silently produced an empty model"; it's kept as a standalone
 * recursive scan mainly to flag the deliberately-unhandled cases (a wrapped
 * `<DataSet>`/`<GSEControl>`, never observed in practice - see
 * processControlBlockStoragePrivate's own doc comment) so those don't go
 * silently unnoticed either.
 */
static bool
containsPrivateControlBlockStorage(mxml_node_t* node) {
    for (mxml_node_t* child = firstElementChild(node); child; child = nextElementSibling(child)) {
        if (isElement(child, "Private") && isControlBlockStoragePrivate(child)) return true;
        if (containsPrivateControlBlockStorage(child)) return true;
    }
    return false;
}

/*
 * Shared fopen -> mxmlLoadFile -> firstElementChild sequence, used by both
 * IedModelSclLoader_load and IedModelSclLoader_listIedNames. *outTree is
 * only meaningful (non-NULL) on success; caller owns it (mxmlDelete).
 */
static mxml_node_t*
loadSclRoot(const char* path, mxml_node_t** outTree, IedModelLoadError* outError) {
    *outError = IED_MODEL_OK;
    *outTree = NULL;

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

    /* Find the <SCL> element specifically, rather than trusting "the first
     * element-typed child" - this vendored Mini-XML has no distinct comment
     * node type at all (confirmed against mxml.h's own mxml_type_t enum:
     * MXML_ELEMENT/INTEGER/OPAQUE/REAL/TEXT/CUSTOM, nothing for comments), so
     * a leading top-level <!--comment--> (confirmed present in a real ABB
     * export) is itself represented as an MXML_ELEMENT node, with
     * mxmlGetElement() returning its raw "!--...--" text. firstElementChild()
     * alone would silently return that comment as if it were the SCL root,
     * leaving every subsequent lookup searching inside an empty node and
     * finding nothing (surfaced as a spurious IED_MODEL_ERR_IED_NOT_FOUND,
     * not an XML parse error, which made the real cause easy to miss). Every
     * other loop in this file already filters by explicit tag name via
     * isElement() and is unaffected - this was the one place that didn't. */
    mxml_node_t* sclRoot = NULL;
    for (mxml_node_t* candidate = firstElementChild(tree); candidate; candidate = nextElementSibling(candidate)) {
        if (isElement(candidate, "SCL")) {
            sclRoot = candidate;
            break;
        }
    }
    if (!sclRoot) {
        *outError = IED_MODEL_ERR_XML_PARSE;
        mxmlDelete(tree);
        return NULL;
    }

    *outTree = tree;
    return sclRoot;
}

LinkedList
IedModelSclLoader_listIedNames(const char* path, IedModelLoadError* outError) {
    mxml_node_t* tree;
    mxml_node_t* sclRoot = loadSclRoot(path, &tree, outError);
    if (!sclRoot) return NULL;

    LinkedList names = LinkedList_create();
    if (!names) {
        *outError = IED_MODEL_ERR_OUT_OF_MEMORY;
        mxmlDelete(tree);
        return NULL;
    }

    for (mxml_node_t* node = firstElementChild(sclRoot); node; node = nextElementSibling(node)) {
        if (!isElement(node, "IED")) continue;
        const char* name = IedModelUtils_attrOrDefault(node, "name", "");
        LinkedList_add(names, strdup(name));
    }

    mxmlDelete(tree);
    return names;
}

IedModel*
IedModelSclLoader_load(const char* path, const char* iedName, IedModelLoadError* outError,
        LinkedList* outDaSemantics, LinkedList* outLnCategories, LinkedList* outDaDescriptions,
        int* outDynDataSetMax, int* outDynDataSetMaxAttributes,
        int* outConfDataSetMax, int* outConfDataSetMaxAttributes) {
    if (outDaSemantics) *outDaSemantics = NULL;
    if (outLnCategories) *outLnCategories = NULL;
    if (outDaDescriptions) *outDaDescriptions = NULL;
    if (outDynDataSetMax) *outDynDataSetMax = -1;
    if (outDynDataSetMaxAttributes) *outDynDataSetMaxAttributes = -1;
    if (outConfDataSetMax) *outConfDataSetMax = -1;
    if (outConfDataSetMaxAttributes) *outConfDataSetMaxAttributes = -1;

    mxml_node_t* tree;
    mxml_node_t* sclRoot = loadSclRoot(path, &tree, outError);
    if (!sclRoot) return NULL;

    mxml_node_t* iedNode = mxmlFindElement(sclRoot, sclRoot, "IED", "name", iedName, MXML_DESCEND);
    if (!iedNode) {
        *outError = IED_MODEL_ERR_IED_NOT_FOUND;
        mxmlDelete(tree);
        return NULL;
    }

    /* <Services> is a direct child of <IED>, a sibling of <AccessPoint> - not
     * nested inside the AccessPoint/Server tree the two loops below walk. */
    mxml_node_t* servicesNode = findFirstChildElement(iedNode, "Services");
    mxml_node_t* dynDataSetNode = servicesNode ? findFirstChildElement(servicesNode, "DynDataSet") : NULL;
    if (dynDataSetNode) {
        if (outDynDataSetMax) *outDynDataSetMax = IedModelUtils_attrInt(dynDataSetNode, "max", -1);
        if (outDynDataSetMaxAttributes)
            *outDynDataSetMaxAttributes = IedModelUtils_attrInt(dynDataSetNode, "maxAttributes", -1);
    }

    mxml_node_t* confDataSetNode = servicesNode ? findFirstChildElement(servicesNode, "ConfDataSet") : NULL;
    if (confDataSetNode) {
        if (outConfDataSetMax) *outConfDataSetMax = IedModelUtils_attrInt(confDataSetNode, "max", -1);
        if (outConfDataSetMaxAttributes)
            *outConfDataSetMaxAttributes = IedModelUtils_attrInt(confDataSetNode, "maxAttributes", -1);
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

    /* Model-scoped (unlike enumAttrs, which is LN-scoped, created/destroyed
     * per-LN inside buildLogicalNodeStructure) - lives for this whole load,
     * handed back to the caller at the end. */
    LinkedList daSemantics = LinkedList_create();
    LinkedList lnCategories = LinkedList_create();
    LinkedList daDescriptions = LinkedList_create();

    LoaderContext ctx = { .model = model, .sclRoot = sclRoot, .templates = templates, .iedName = iedName,
        .daSemantics = daSemantics, .lnCategories = lnCategories, .daDescriptions = daDescriptions };

    for (mxml_node_t* apNode = firstElementChild(iedNode); apNode; apNode = nextElementSibling(apNode)) {
        if (!isElement(apNode, "AccessPoint")) continue;
        buildAccessPointStructure(apNode, &ctx);
    }

    for (mxml_node_t* apNode = firstElementChild(iedNode); apNode; apNode = nextElementSibling(apNode)) {
        if (!isElement(apNode, "AccessPoint")) continue;
        buildAccessPointReferences(apNode, &ctx);
    }

    if (!model->rcbs && !model->gseCBs && !model->dataSets && containsPrivateControlBlockStorage(iedNode)) {
        fprintf(stderr,
                "[ied_model] WARN: IED '%s' has zero ReportControl/GSEControl/DataSet elements but "
                "contains vendor <Private type=\"...ControlBlockStorage...\"> element(s) - this loader "
                "unescapes and parses a <ReportControl> found this way (see processControlBlockStoragePrivate), "
                "but every one here either failed to parse/resolve or wraps a <DataSet>/<GSEControl> instead, "
                "which are not handled. The resulting IedModel has no report/GOOSE targets at all.\n", iedName);
    }

    mxmlDelete(tree);
    if (outDaSemantics) *outDaSemantics = daSemantics;
    else LinkedList_destroyDeep(daSemantics, free); /* caller doesn't want it - don't leak */
    if (outLnCategories) *outLnCategories = lnCategories;
    else LinkedList_destroyDeep(lnCategories, free);
    if (outDaDescriptions) *outDaDescriptions = daDescriptions;
    else LinkedList_destroyDeep(daDescriptions, freeDaDescEntry);
    return model;
}
