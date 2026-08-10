#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model/domain/ied_model_usecases.h"
#include "features/ied_model/utils/ied_model_ln_category.h"
#include "iec61850_dynamic_model.h"
#include "iec61850_common.h"
#include "mms_common.h"

/* ---- recursive tree walkers over the built model ---- */

static void
collectDataAttributesByFc(ModelNode* node, FunctionalConstraint fc, LinkedList result) {
    if (ModelNode_getType(node) == DataAttributeModelType) {
        if (((DataAttribute*) node)->fc == fc) {
            char* ref = ModelNode_getObjectReference(node, NULL);
            if (ref) {
                LinkedList_add(result, ref);
            } else {
                fprintf(stderr, "[ied_model] WARN: could not build object reference for DA '%s' - omitted from read targets\n",
                        node->name);
            }
        }
        /* DataAttributes are leaves for this purpose: nested BDAs of a Struct inherit
         * the same fc, so if the parent didn't match, its children won't either, and
         * if it did match, the parent reference already covers them. */
        return;
    }

    LinkedList children = ModelNode_getChildren(node);
    if (children) {
        LinkedList element = LinkedList_getNext(children);
        while (element) {
            collectDataAttributesByFc((ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        /* Static: children are live ModelNode pointers owned by the model, not ours to free. */
        LinkedList_destroyStatic(children);
    }
}

static void
collectControllableDataObjects(ModelNode* node, LinkedList result) {
    if (ModelNode_getType(node) == DataObjectModelType) {
        if (DataObject_hasFCData((DataObject*) node, IEC61850_FC_CO)) {
            char* ref = ModelNode_getObjectReference(node, NULL);
            if (ref) {
                LinkedList_add(result, ref);
            } else {
                fprintf(stderr,
                        "[ied_model] WARN: could not build object reference for controllable DO '%s' - omitted from control targets\n",
                        node->name);
            }
            return; /* report the controllable DO itself, not its nested SDOs separately */
        }
    }

    LinkedList children = ModelNode_getChildren(node);
    if (children) {
        LinkedList element = LinkedList_getNext(children);
        while (element) {
            collectControllableDataObjects((ModelNode*) LinkedList_getData(element), result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
    }
}

/*
 * Unlike collectDataAttributesByFc (which terminal-izes any DataAttribute
 * node, since it's building single read-target references where a
 * CONSTRUCTED attribute's own BDAs are covered by the parent's reference),
 * this walk keeps going: a CONSTRUCTED Data Attribute's BDA children are
 * recursed into individually, because on the wire a CONSTRUCTED DA is itself
 * an MMS_STRUCTURE - decomposing a DO-level dataset entry into flat leaf
 * points requires reaching genuinely terminal (basic-typed) attributes, not
 * stopping at the first CONSTRUCTED one. FC filtering still happens once at
 * the DataAttribute level (BDAs inherit their parent DA's fc) - a DO can mix
 * attributes of different FCs as direct children (e.g. a DPC's stVal/q/t at
 * ST alongside Oper/SBOw/Cancel at CO), so filtering can't happen any higher.
 *
 * Builds the "$"-joined MMS-variable-name-style path itself (pathPrefix +
 * "$" + node->name at each level) rather than using
 * ModelNode_getObjectReference - that function produces the DOT-separated
 * ACSI object-reference style ("LD/LN.DO.DA"), which does not match the
 * "$"-joined style every dataset-member reference in this codebase already
 * uses (see IedModelUseCases_getDataSetMemberReferences/
 * IedModelUtils_buildFcdaVariableName) and that ipc_dispatcher's
 * quality-pairing logic specifically parses (splits on the LAST "$").
 */
static void
collectLeafReferencesByFc(ModelNode* node, FunctionalConstraint fc, const char* pathPrefix, LinkedList result) {
    size_t pathLen = strlen(pathPrefix) + 1 + strlen(node->name) + 1;
    char* nodePath = malloc(pathLen);
    if (!nodePath) return;
    snprintf(nodePath, pathLen, "%s$%s", pathPrefix, node->name);

    if (ModelNode_getType(node) == DataAttributeModelType) {
        if (((DataAttribute*) node)->fc != fc) {
            free(nodePath);
            return;
        }

        LinkedList children = ModelNode_getChildren(node);
        LinkedList firstChild = children ? LinkedList_getNext(children) : NULL;

        if (!firstChild) {
            /* Basic-typed leaf - genuinely terminal. Ownership of nodePath
             * transfers to result. */
            LinkedList_add(result, nodePath);
            if (children) LinkedList_destroyStatic(children);
            return;
        }

        /* CONSTRUCTED attribute (e.g. a WYE phase's cVal) - recurse into its
         * BDA children instead of terminal-izing here. */
        LinkedList element = firstChild;
        while (element) {
            collectLeafReferencesByFc((ModelNode*) LinkedList_getData(element), fc, nodePath, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
        free(nodePath);
        return;
    }

    /* DataObjectModelType (a nested SDO) - recurse unconditionally; FC
     * filtering happens once a DataAttribute leaf is reached. */
    LinkedList children = ModelNode_getChildren(node);
    if (children) {
        LinkedList element = LinkedList_getNext(children);
        while (element) {
            collectLeafReferencesByFc((ModelNode*) LinkedList_getData(element), fc, nodePath, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
    }
    free(nodePath);
}

/* ---- member-reference string helpers (shared by DataSet-indexed accessors
 * and their memberReference-keyed counterparts below) ---- */

/* Builds one DataSetEntry's own "LD/LN$FC$DO[$DA]" member-reference string -
 * this codebase's standard dataset-member-reference convention (see
 * IedModelUseCases_getDataSetMemberReferences's own doc comment). Returns
 * NULL if the entry is missing either half. */
static char*
buildMemberReferenceForEntry(DataSetEntry* entry) {
    if (!entry->logicalDeviceName || !entry->variableName) return NULL;
    size_t len = strlen(entry->logicalDeviceName) + 1 + strlen(entry->variableName) + 1;
    char* ref = malloc(len);
    if (ref) snprintf(ref, len, "%s/%s", entry->logicalDeviceName, entry->variableName);
    return ref;
}

/* Resolves dataset member `memberIndex`'s own member-reference string - the
 * shared first step every DataSet-indexed accessor below needs before it can
 * delegate to the equivalent memberReference-keyed function. Returns NULL
 * (never a partial string) on any failure: NULL datasetReference, negative
 * memberIndex, an unresolved dataset, an index past the entry list, or an
 * entry missing logicalDeviceName/variableName. Caller owns a non-NULL
 * result. */
static char*
resolveDataSetMemberReference(IedModelHandle handle, const char* datasetReference, int memberIndex) {
    if (!datasetReference || memberIndex < 0) return NULL;

    DataSet* dataSet = IedModel_lookupDataSet(handle->model, datasetReference);
    if (!dataSet) return NULL;

    DataSetEntry* entry = DataSet_getFirstEntry(dataSet);
    for (int i = 0; entry && i < memberIndex; i++) entry = DataSetEntry_getNext(entry);
    if (!entry) return NULL;

    return buildMemberReferenceForEntry(entry);
}

/*
 * Parses memberReference ("LD/LN$FC$DO[$SDO...][$DA]" - this codebase's own
 * dataset-member-reference convention) and resolves it to its DO-level
 * ModelNode pointer and FunctionalConstraint - the shared resolution step every Gap-4
 * decomposition accessor needs (leaf references/wire types/semantics),
 * whether reached via a DataSetEntry already registered in this model (the
 * DataSet-indexed accessors, via resolveDataSetMemberReference above) or
 * directly from a plain reference string with no backing DataSet at all (the
 * *ForMemberReference accessors below - needed because a dataset resolved
 * live over the wire, e.g. mms_report_client's tier-2 "pulled" dataset
 * handling, has no DataSetEntry/DataSet object registered in this IedModel to
 * read logicalDeviceName/variableName from in the first place).
 *
 * Returns true only when memberReference has EXACTLY 3 "$"-segments after its
 * "LD/LN" prefix (LN$FC$DO, no trailing daName - the one shape Gap 4
 * decomposition ever applies to; daName present means "already leaf-level,
 * nothing to decompose" here, same as every pre-existing decomposition
 * accessor) AND that LD/LN/DO chain resolves to a real DataObjectModelType
 * node in this model. Any other shape or an unresolved chain returns false
 * with *outDoNode / *outFc untouched.
 */
static bool
resolveDoLevelMemberReference(IedModel* model, const char* memberReference, ModelNode** outDoNode,
        FunctionalConstraint* outFc) {
    if (!memberReference) return false;

    char* copy = strdup(memberReference);
    if (!copy) return false;

    bool ok = false;
    char* slash = strchr(copy, '/');
    if (slash) {
        *slash = '\0';
        char* lnToken = strtok(slash + 1, "$");
        char* fcToken = lnToken ? strtok(NULL, "$") : NULL;
        char* doToken = fcToken ? strtok(NULL, "$") : NULL;
        char* daToken = doToken ? strtok(NULL, "$") : NULL;

        if (doToken && !daToken) {
            LogicalDevice* ld = IedModel_getDevice(model, copy);
            LogicalNode* ln = ld ? LogicalDevice_getLogicalNode(ld, lnToken) : NULL;
            ModelNode* doNode = ln ? ModelNode_getChild((ModelNode*) ln, doToken) : NULL;

            if (doNode && ModelNode_getType(doNode) == DataObjectModelType) {
                *outDoNode = doNode;
                *outFc = FunctionalConstraint_fromString(fcToken);
                ok = true;
            }
        }
    }

    free(copy);
    return ok;
}

/*
 * Looks up `ln`'s classified LnCategory in handle->lnCategories (small N - a
 * linear scan once per target getter call, not a hot path - same posture as
 * the existing `da ==` semantic lookup at getSemanticForMemberReference
 * below). Degrades to IED_MODEL_LN_CATEGORY_OTHER, never crashes/guesses, if
 * `ln` has no entry (shouldn't happen in practice - every LN built by either
 * loader gets one - but a missing entry must never silently pass every
 * filter, which OTHER alone as a lone bit would risk if a caller filtered on
 * exactly OTHER; treating "not found" as "matches nothing but OTHER" is the
 * conservative choice).
 */
static LnCategory
categoryForLn(IedModelHandle handle, LogicalNode* ln) {
    for (int i = 0; i < handle->lnCategoryCount; i++) {
        if (handle->lnCategories[i].ln == ln) return handle->lnCategories[i].category;
    }
    return IED_MODEL_LN_CATEGORY_OTHER;
}

/*
 * Same lookup as categoryForLn, for IedModelLnCategoryEntry.alwaysInclude
 * (see its own doc comment, and IedModelLnCategory_isAlwaysIncludedLnClass's).
 * Degrades to false on a missing entry - the OPPOSITE default from
 * categoryForLn's OTHER, and deliberately so: "not found" must never
 * fabricate an exemption that lets an unclassified LN bypass every filter.
 * An LLN0 with no entry therefore just filters as OTHER, which is exactly the
 * pre-exemption behavior, never worse.
 */
static bool
alwaysIncludeForLn(IedModelHandle handle, LogicalNode* ln) {
    for (int i = 0; i < handle->lnCategoryCount; i++) {
        if (handle->lnCategories[i].ln == ln) return handle->lnCategories[i].alwaysInclude;
    }
    return false;
}

/* "|"-joins every set category bit's own name into buf, e.g.
 * "CONTROL|MEASUREMENT" - diagnostic-only (the whole-device filter exclusion
 * log below), never a wire format, so a fixed-size stack buffer is fine (at
 * most "CONTROL|MEASUREMENT|PROTECTION|OTHER" ever needs to fit). */
static void
formatCategoryMask(LnCategoryMask mask, char* buf, size_t bufSize) {
    static const LnCategory kAllCategories[] = { IED_MODEL_LN_CATEGORY_CONTROL, IED_MODEL_LN_CATEGORY_MEASUREMENT,
        IED_MODEL_LN_CATEGORY_PROTECTION, IED_MODEL_LN_CATEGORY_OTHER };
    buf[0] = '\0';
    bool first = true;
    for (size_t i = 0; i < sizeof(kAllCategories) / sizeof(kAllCategories[0]); i++) {
        if (!(mask & kAllCategories[i])) continue;
        if (!first) strncat(buf, "|", bufSize - strlen(buf) - 1);
        strncat(buf, IedModelLnCategory_toString(kAllCategories[i]), bufSize - strlen(buf) - 1);
        first = false;
    }
    if (first) strncat(buf, "(none)", bufSize - strlen(buf) - 1);
}

/* ---- public use-cases ---- */

LinkedList
IedModelUseCases_getGooseSubscriptionTargets(IedModelHandle handle) {
    LinkedList result = LinkedList_create();

    /* categoryFilter deliberately does NOT gate GoCB visibility - the daemon
     * always needs to know where every GoCB/dataset lives regardless of
     * category, since filtering here would make an entire GoCB disappear
     * whenever its PARENT LN's category didn't match, even though its
     * dataset commonly carries other LNs' data (real devices very often
     * parent every GoCB on LLN0, which has nothing to do with what its
     * dataset actually reports on). The filter instead applies per-point,
     * downstream in goose_subscriber's own candidate collection, against
     * each individual value's OWN LN category - see
     * GooseSubscriberMemberRefCache.categoryFilter. */
    for (GSEControlBlock* gcb = handle->model->gseCBs; gcb; gcb = gcb->sibling) {
        char* lnRef = ModelNode_getObjectReference((ModelNode*) gcb->parent, NULL);
        if (!lnRef) {
            fprintf(stderr, "[ied_model] WARN: could not build parent LN reference for GoCB '%s' - omitted from GOOSE targets\n",
                    gcb->name);
            continue;
        }

        /* GoCB reference notation uses "$"-separated MMS style, per goose_subscriber.h's
         * documented example ("simpleIOGenericIO/LLN0$GO$gcbEvents"). */
        size_t len = strlen(lnRef) + strlen("$GO$") + strlen(gcb->name) + 1;
        char* ref = malloc(len);
        if (ref) snprintf(ref, len, "%s$GO$%s", lnRef, gcb->name);

        /* Dataset object-reference notation, "$"-joined, mirrors
         * getReportSubscriptionTargets's datasetRef convention above. */
        char* datasetRef = NULL;
        if (gcb->dataSetName && gcb->dataSetName[0] != '\0') {
            size_t dsLen = strlen(lnRef) + strlen("$") + strlen(gcb->dataSetName) + 1;
            datasetRef = malloc(dsLen);
            if (datasetRef) snprintf(datasetRef, dsLen, "%s$%s", lnRef, gcb->dataSetName);
        }
        free(lnRef);

        if (!ref) {
            free(datasetRef);
            continue;
        }

        GooseSubscriptionTarget* target = calloc(1, sizeof(GooseSubscriptionTarget));
        if (!target) {
            free(ref);
            free(datasetRef);
            continue;
        }
        target->objectReference = ref;
        target->datasetReference = datasetRef;

        if (gcb->address) {
            target->hasAddress = true;
            target->vlanId = gcb->address->vlanId;
            target->vlanPriority = gcb->address->vlanPriority;
            target->appId = gcb->address->appId;
            memcpy(target->dstMac, gcb->address->dstAddress, 6);
        }

        LinkedList_add(result, target);
    }

    return result;
}

void
IedModelUseCases_destroyGooseSubscriptionTarget(void* target) {
    if (!target) return;
    GooseSubscriptionTarget* gooseTarget = (GooseSubscriptionTarget*) target;
    free(gooseTarget->objectReference);
    free(gooseTarget->datasetReference);
    free(gooseTarget);
}

LinkedList
IedModelUseCases_getReportSubscriptionTargets(IedModelHandle handle) {
    LinkedList result = LinkedList_create();

    /* categoryFilter deliberately does NOT gate RCB visibility - same
     * rationale as getGooseSubscriptionTargets above. Real SCL very commonly
     * parents every RCB on LLN0 (category OTHER) regardless of what its
     * dataset actually reports on (confirmed against a real DIGSI 5/SIPROTEC
     * 6MD85 station file: 100% of RCBs/GoCBs parented on LLN0, dataset
     * members spanning XCBR/MMXU/PTRC/etc.) - gating the whole RCB here would
     * make the filter useless on that hardware. The daemon always needs
     * every RCB visible so it always knows where every dataset lives and can
     * enable reporting normally; the filter applies per-point instead, in
     * mms_report_client's own candidate collection, against each value's OWN
     * LN category - see MmsReportClientMemberRefCacheEntry.categoryFilter. */
    for (ReportControlBlock* rcb = handle->model->rcbs; rcb; rcb = rcb->sibling) {
        char* lnRef = ModelNode_getObjectReference((ModelNode*) rcb->parent, NULL);
        if (!lnRef) {
            fprintf(stderr, "[ied_model] WARN: could not build parent LN reference for RCB '%s' - omitted from report targets\n",
                    rcb->name);
            continue;
        }

        /* RCB reference notation uses object-reference dot style with a "BR"
         * segment for buffered RCBs and "RP" for unbuffered ones, per
         * IedConnection_getRCBValues's documented convention
         * (third_party/include/iec61850_client.h). */
        const char* fcSegment = rcb->buffered ? ".BR." : ".RP.";
        size_t refLen = strlen(lnRef) + strlen(fcSegment) + strlen(rcb->name) + 1;
        char* ref = malloc(refLen);
        if (ref) snprintf(ref, refLen, "%s%s%s", lnRef, fcSegment, rcb->name);

        /* Dataset object-reference notation, "$"-joined, mirrors the existing
         * GOOSE-reference convention above. */
        char* datasetRef = NULL;
        if (rcb->dataSetName && rcb->dataSetName[0] != '\0') {
            size_t dsLen = strlen(lnRef) + strlen("$") + strlen(rcb->dataSetName) + 1;
            datasetRef = malloc(dsLen);
            if (datasetRef) snprintf(datasetRef, dsLen, "%s$%s", lnRef, rcb->dataSetName);
        }

        if (!ref) {
            free(lnRef);
            free(datasetRef);
            continue;
        }

        ReportControlBlockTarget* target = malloc(sizeof(ReportControlBlockTarget));
        if (!target) {
            free(lnRef);
            free(ref);
            free(datasetRef);
            continue;
        }
        target->objectReference = ref;
        target->buffered = rcb->buffered;
        target->datasetReference = datasetRef;
        target->lnReference = lnRef;

        LinkedList_add(result, target);
    }

    return result;
}

void
IedModelUseCases_destroyReportControlBlockTarget(void* target) {
    if (!target) return;
    ReportControlBlockTarget* rcbTarget = (ReportControlBlockTarget*) target;
    free(rcbTarget->objectReference);
    free(rcbTarget->datasetReference);
    free(rcbTarget->lnReference);
    free(rcbTarget);
}

LinkedList
IedModelUseCases_getDataSetMemberReferences(IedModelHandle handle, const char* datasetReference) {
    LinkedList result = LinkedList_create();
    if (!datasetReference) return result;

    DataSet* dataSet = IedModel_lookupDataSet(handle->model, datasetReference);
    if (!dataSet) {
        fprintf(stderr, "[ied_model] WARN: dataset '%s' not found in model - returning empty member list "
                "(check for a typo'd/mismatched datSet reference on the referencing RCB/GoCB)\n", datasetReference);
        return result;
    }

    for (DataSetEntry* entry = DataSet_getFirstEntry(dataSet); entry; entry = DataSetEntry_getNext(entry)) {
        char* ref = buildMemberReferenceForEntry(entry);
        if (ref) LinkedList_add(result, ref);
    }
    return result;
}

/*
 * Member-reference-keyed counterpart of IedModelUseCases_getDataSetMemberLeafReferences,
 * resolved directly from memberReference ("LD/LN$FC$DO[$DA]") rather than a
 * (datasetReference, index) pair into a registered DataSet - see
 * resolveDoLevelMemberReference's own doc comment for why this exists (a
 * dataset resolved live over the wire, with no DataSet object registered in
 * this IedModel, still needs this same decomposition). Same "empty list if
 * not decomposed" contract. memberReference itself is reused verbatim as the
 * decomposition base path passed to collectLeafReferencesByFc - by
 * construction it's already exactly "LD/LN$FC$DO" whenever this call
 * succeeds (resolveDoLevelMemberReference only returns true for that exact
 * shape), the same base path IedModelUseCases_getDataSetMemberLeafReferences
 * itself builds from an entry's own logicalDeviceName/variableName.
 */
LinkedList
IedModelUseCases_getLeafReferencesForMemberReference(IedModelHandle handle, const char* memberReference) {
    LinkedList result = LinkedList_create();
    ModelNode* doNode = NULL;
    FunctionalConstraint fc = IEC61850_FC_NONE;
    if (!resolveDoLevelMemberReference(handle->model, memberReference, &doNode, &fc)) return result;

    LinkedList doChildren = ModelNode_getChildren(doNode);
    if (doChildren) {
        LinkedList element = LinkedList_getNext(doChildren);
        while (element) {
            collectLeafReferencesByFc((ModelNode*) LinkedList_getData(element), fc, memberReference, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(doChildren);
    }
    return result;
}

LinkedList
IedModelUseCases_getDataSetMemberLeafReferences(IedModelHandle handle, const char* datasetReference,
        int memberIndex) {
    char* memberRef = resolveDataSetMemberReference(handle, datasetReference, memberIndex);
    if (!memberRef) return LinkedList_create();

    LinkedList result = IedModelUseCases_getLeafReferencesForMemberReference(handle, memberRef);
    free(memberRef);
    return result;
}

/* ---- Dbpos semantic lookup (see IedModelDaSemantic's own doc comment) ---- */

static IedModelDaSemantic
lookupDaSemantic(IedModelHandle handle, DataAttribute* da) {
    if (!da) return IED_MODEL_DA_SEMANTIC_NONE;
    for (int i = 0; i < handle->daSemanticCount; i++) {
        if (handle->daSemantics[i].da == da) return handle->daSemantics[i].semantic;
    }
    return IED_MODEL_DA_SEMANTIC_NONE;
}

/*
 * Resolves an already leaf-level FCDA's daToken down to its real terminal
 * DataAttribute* node. daToken is the raw SCL @daName attribute value as
 * embedded (unmodified) into the "$"-joined wire reference by
 * IedModelUtils_buildFcdaVariableName - per IEC 61850-6, this can itself be
 * a "."-separated path into nested BDA/SDO levels (e.g. "cVal.mag.f"), NOT
 * "$"-separated (that convention is reserved for the top-level
 * LN$FC$DO$daName segments elsewhere in this codebase - see
 * collectLeafReferencesByFc's own comment on why decomposed leaves use "$"
 * per level instead: that walk builds paths from the MODEL TREE structure,
 * while this one is splitting one ALREADY-GIVEN SCL attribute string). Not
 * currently exercised by any real fixture in this repo (a flat, undotted
 * daName is the only case seen so far) but handled correctly regardless,
 * rather than assuming daToken is always a single segment. Returns NULL on
 * any resolution failure (unresolved LD/LN/DO/BDA segment, or the resolved
 * node isn't actually a DataAttribute) - never guesses.
 */
static DataAttribute*
resolveTerminalDataAttribute(IedModel* model, const char* ldName, const char* lnToken, const char* doToken,
        const char* daToken) {
    LogicalDevice* ld = IedModel_getDevice(model, ldName);
    LogicalNode* ln = ld ? LogicalDevice_getLogicalNode(ld, lnToken) : NULL;
    ModelNode* node = ln ? ModelNode_getChild((ModelNode*) ln, doToken) : NULL;
    if (!node || !daToken) return NULL;

    char* daCopy = strdup(daToken);
    if (!daCopy) return NULL;

    char* seg = strtok(daCopy, ".");
    while (seg && node) {
        node = ModelNode_getChild(node, seg);
        seg = strtok(NULL, ".");
    }
    free(daCopy);

    if (!node || ModelNode_getType(node) != DataAttributeModelType) return NULL;
    return (DataAttribute*) node;
}

/* Semantics-lookup sibling of collectLeafReferencesByFc - identical
 * traversal (same FC filtering, same CONSTRUCTED-attribute recursion), but
 * appends this handle's resolved IedModelDaSemantic for each genuinely
 * terminal leaf instead of building a reference string. Order matches
 * collectLeafReferencesByFc's own traversal exactly (same walk), so results
 * stay index-aligned with IedModelUseCases_getDataSetMemberLeafReferences's
 * own output for the same (datasetReference, memberIndex). */
static void
collectLeafSemanticsByFc(IedModelHandle handle, ModelNode* node, FunctionalConstraint fc, LinkedList result) {
    if (ModelNode_getType(node) == DataAttributeModelType) {
        if (((DataAttribute*) node)->fc != fc) return;

        LinkedList children = ModelNode_getChildren(node);
        LinkedList firstChild = children ? LinkedList_getNext(children) : NULL;

        if (!firstChild) {
            IedModelDaSemantic* boxed = malloc(sizeof(IedModelDaSemantic));
            if (boxed) {
                *boxed = lookupDaSemantic(handle, (DataAttribute*) node);
                LinkedList_add(result, boxed);
            }
            if (children) LinkedList_destroyStatic(children);
            return;
        }

        LinkedList element = firstChild;
        while (element) {
            collectLeafSemanticsByFc(handle, (ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
        return;
    }

    LinkedList children = ModelNode_getChildren(node);
    if (children) {
        LinkedList element = LinkedList_getNext(children);
        while (element) {
            collectLeafSemanticsByFc(handle, (ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
    }
}

/*
 * Member-reference-keyed counterpart of IedModelUseCases_getDataSetMemberSemantics'
 * per-entry resolution - the Dbpos semantic of memberReference itself when
 * it's already leaf-level (has a trailing "$DA" segment). Resolved directly
 * from the reference string rather than a DataSetEntry, same rationale as
 * resolveDoLevelMemberReference's own doc comment (a pulled dataset has no
 * DataSetEntry to read logicalDeviceName/variableName from). Returns
 * IED_MODEL_DA_SEMANTIC_NONE if memberReference is NULL, malformed, DO-level
 * (no daToken - nothing to look up, use *_getLeafSemanticsForMemberReference
 * instead), or doesn't resolve in this model - same graceful-degradation
 * posture as every other semantic lookup in this file.
 */
IedModelDaSemantic
IedModelUseCases_getSemanticForMemberReference(IedModelHandle handle, const char* memberReference) {
    if (!memberReference) return IED_MODEL_DA_SEMANTIC_NONE;

    char* copy = strdup(memberReference);
    if (!copy) return IED_MODEL_DA_SEMANTIC_NONE;

    IedModelDaSemantic semantic = IED_MODEL_DA_SEMANTIC_NONE;
    char* slash = strchr(copy, '/');
    if (slash) {
        *slash = '\0';
        char* lnToken = strtok(slash + 1, "$");
        char* fcToken = lnToken ? strtok(NULL, "$") : NULL;
        char* doToken = fcToken ? strtok(NULL, "$") : NULL;
        char* daToken = doToken ? strtok(NULL, "$") : NULL;

        if (doToken && daToken) {
            DataAttribute* da = resolveTerminalDataAttribute(handle->model, copy, lnToken, doToken, daToken);
            semantic = lookupDaSemantic(handle, da);
        }
    }

    free(copy);
    return semantic;
}

/*
 * Member-reference-keyed LN category lookup - resolves only as far as the
 * "LD/LN" prefix (category is constant across every leaf under one LN,
 * unlike a Dbpos semantic), then defers to categoryForLn. Returns
 * IED_MODEL_LN_CATEGORY_OTHER on any resolution failure - never a guess into
 * a real category.
 */
LnCategory
IedModelUseCases_getCategoryForMemberReference(IedModelHandle handle, const char* memberReference) {
    if (!handle || !memberReference) return IED_MODEL_LN_CATEGORY_OTHER;

    char* copy = strdup(memberReference);
    if (!copy) return IED_MODEL_LN_CATEGORY_OTHER;

    LnCategory category = IED_MODEL_LN_CATEGORY_OTHER;
    char* slash = strchr(copy, '/');
    if (slash) {
        *slash = '\0';
        char* lnToken = strtok(slash + 1, "$");
        LogicalDevice* ld = IedModel_getDevice(handle->model, copy);
        LogicalNode* ln = (ld && lnToken) ? LogicalDevice_getLogicalNode(ld, lnToken) : NULL;
        if (ln) category = categoryForLn(handle, ln);
    }

    free(copy);
    return category;
}

/*
 * alwaysInclude counterpart of getCategoryForMemberReference above - same
 * "LD/LN" prefix parse (the exemption is a per-LN property, exactly like
 * category), deferring to alwaysIncludeForLn. Returns false on any
 * resolution failure - never fabricates an exemption out of a reference it
 * couldn't resolve.
 */
bool
IedModelUseCases_isMemberReferenceAlwaysIncluded(IedModelHandle handle, const char* memberReference) {
    if (!handle || !memberReference) return false;

    char* copy = strdup(memberReference);
    if (!copy) return false;

    bool alwaysInclude = false;
    char* slash = strchr(copy, '/');
    if (slash) {
        *slash = '\0';
        char* lnToken = strtok(slash + 1, "$");
        LogicalDevice* ld = IedModel_getDevice(handle->model, copy);
        LogicalNode* ln = (ld && lnToken) ? LogicalDevice_getLogicalNode(ld, lnToken) : NULL;
        if (ln) alwaysInclude = alwaysIncludeForLn(handle, ln);
    }

    free(copy);
    return alwaysInclude;
}

/* ---- desc lookup (see IedModelDaDescEntry's own doc comment) ---- */

static const char*
lookupDaDescription(IedModelHandle handle, DataAttribute* da) {
    if (!da) return NULL;
    for (int i = 0; i < handle->daDescriptionCount; i++) {
        if (handle->daDescriptions[i].da == da) return handle->daDescriptions[i].desc;
    }
    return NULL;
}

/*
 * Member-reference-keyed counterpart of IedModelUseCases_getDataSetMemberSemantics'
 * per-entry resolution, mirroring getSemanticForMemberReference's own shape
 * exactly (same "LD/LN$FC$DO$DA" parse, same resolveTerminalDataAttribute
 * call) but looking up a captured desc string instead of a Dbpos semantic.
 * Returns NULL (borrowed - never free) if memberReference is NULL, malformed,
 * DO-level (no daToken), doesn't resolve, or genuinely has no captured desc.
 */
const char*
IedModelUseCases_getDescriptionForMemberReference(IedModelHandle handle, const char* memberReference) {
    if (!memberReference) return NULL;

    char* copy = strdup(memberReference);
    if (!copy) return NULL;

    const char* desc = NULL;
    char* slash = strchr(copy, '/');
    if (slash) {
        *slash = '\0';
        char* lnToken = strtok(slash + 1, "$");
        char* fcToken = lnToken ? strtok(NULL, "$") : NULL;
        char* doToken = fcToken ? strtok(NULL, "$") : NULL;
        char* daToken = doToken ? strtok(NULL, "$") : NULL;

        if (doToken && daToken) {
            DataAttribute* da = resolveTerminalDataAttribute(handle->model, copy, lnToken, doToken, daToken);
            desc = lookupDaDescription(handle, da);
        }
    }

    free(copy);
    return desc;
}

/* Description-lookup sibling of collectLeafSemanticsByFc - identical
 * traversal, but appends this handle's captured desc (borrowed, possibly
 * NULL) for each genuinely terminal leaf instead of a semantic. Order
 * matches collectLeafReferencesByFc's own traversal exactly, so results stay
 * index-aligned with IedModelUseCases_getLeafReferencesForMemberReference's
 * own output for the same memberReference. Elements are raw (possibly NULL)
 * char* - no boxing needed, unlike the enum-valued semantics list, since a
 * string pointer already fits LinkedList's void* payload directly. */
static void
collectLeafDescriptionsByFc(IedModelHandle handle, ModelNode* node, FunctionalConstraint fc, LinkedList result) {
    if (ModelNode_getType(node) == DataAttributeModelType) {
        if (((DataAttribute*) node)->fc != fc) return;

        LinkedList children = ModelNode_getChildren(node);
        LinkedList firstChild = children ? LinkedList_getNext(children) : NULL;

        if (!firstChild) {
            LinkedList_add(result, (void*) lookupDaDescription(handle, (DataAttribute*) node));
            if (children) LinkedList_destroyStatic(children);
            return;
        }

        LinkedList element = firstChild;
        while (element) {
            collectLeafDescriptionsByFc(handle, (ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
        return;
    }

    LinkedList children = ModelNode_getChildren(node);
    if (children) {
        LinkedList element = LinkedList_getNext(children);
        while (element) {
            collectLeafDescriptionsByFc(handle, (ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
    }
}

LinkedList
IedModelUseCases_getLeafDescriptionsForMemberReference(IedModelHandle handle, const char* memberReference) {
    LinkedList result = LinkedList_create();
    ModelNode* doNode = NULL;
    FunctionalConstraint fc = IEC61850_FC_NONE;
    if (!resolveDoLevelMemberReference(handle->model, memberReference, &doNode, &fc)) return result;

    LinkedList doChildren = ModelNode_getChildren(doNode);
    if (doChildren) {
        LinkedList element = LinkedList_getNext(doChildren);
        while (element) {
            collectLeafDescriptionsByFc(handle, (ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(doChildren);
    }
    return result;
}

LinkedList
IedModelUseCases_getDataSetMemberSemantics(IedModelHandle handle, const char* datasetReference) {
    LinkedList result = LinkedList_create();
    if (!datasetReference) return result;

    DataSet* dataSet = IedModel_lookupDataSet(handle->model, datasetReference);
    if (!dataSet) return result;

    for (DataSetEntry* entry = DataSet_getFirstEntry(dataSet); entry; entry = DataSetEntry_getNext(entry)) {
        IedModelDaSemantic semantic = IED_MODEL_DA_SEMANTIC_NONE;

        char* memberRef = buildMemberReferenceForEntry(entry);
        if (memberRef) {
            semantic = IedModelUseCases_getSemanticForMemberReference(handle, memberRef);
            free(memberRef);
        }

        IedModelDaSemantic* boxed = malloc(sizeof(IedModelDaSemantic));
        if (boxed) {
            *boxed = semantic;
            LinkedList_add(result, boxed);
        }
    }
    return result;
}

/*
 * Member-reference-keyed counterpart of IedModelUseCases_getDataSetMemberLeafSemantics -
 * see IedModelUseCases_getLeafReferencesForMemberReference's own doc comment
 * for why this exists (a pulled dataset's members have no backing DataSet
 * object in this model). Index-aligned with
 * IedModelUseCases_getLeafReferencesForMemberReference's own output for the
 * same memberReference (same underlying collectLeafSemanticsByFc/
 * collectLeafReferencesByFc walk).
 */
LinkedList
IedModelUseCases_getLeafSemanticsForMemberReference(IedModelHandle handle, const char* memberReference) {
    LinkedList result = LinkedList_create();
    ModelNode* doNode = NULL;
    FunctionalConstraint fc = IEC61850_FC_NONE;
    if (!resolveDoLevelMemberReference(handle->model, memberReference, &doNode, &fc)) return result;

    LinkedList doChildren = ModelNode_getChildren(doNode);
    if (doChildren) {
        LinkedList element = LinkedList_getNext(doChildren);
        while (element) {
            collectLeafSemanticsByFc(handle, (ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(doChildren);
    }
    return result;
}

LinkedList
IedModelUseCases_getDataSetMemberLeafSemantics(IedModelHandle handle, const char* datasetReference,
        int memberIndex) {
    char* memberRef = resolveDataSetMemberReference(handle, datasetReference, memberIndex);
    if (!memberRef) return LinkedList_create();

    LinkedList result = IedModelUseCases_getLeafSemanticsForMemberReference(handle, memberRef);
    free(memberRef);
    return result;
}

/* Wire-type-lookup sibling of collectLeafReferencesByFc -
 * identical traversal (same FC filtering, same CONSTRUCTED-attribute recursion),
 * but appends each genuinely terminal leaf's own already-known
 * DataAttributeType (set once at SCL-load time via IedModelUtils_mapBType,
 * stored directly on the DataAttribute node - see struct sDataAttribute in
 * iec61850_model.h) instead of building a reference string. Order matches
 * collectLeafReferencesByFc's own traversal exactly (same walk), so results
 * stay index-aligned with IedModelUseCases_getDataSetMemberLeafReferences's
 * own output for the same (datasetReference, memberIndex) - this is what lets
 * mms_report_client/goose_subscriber cross-check each decomposed leaf's
 * ACTUAL wire type against its EXPECTED (SCL-declared) type before trusting
 * the reference-to-value zip (see IedModelUseCases_dataAttributeTypeMatchesMmsType's
 * own doc comment for why: same-count-different-order mismatches between this
 * daemon's locally-resolved leaf order and a real device's actual wire order
 * are NOT caught by the pre-existing count-only fallback). */
static void
collectLeafWireTypesByFc(ModelNode* node, FunctionalConstraint fc, LinkedList result) {
    if (ModelNode_getType(node) == DataAttributeModelType) {
        if (((DataAttribute*) node)->fc != fc) return;

        LinkedList children = ModelNode_getChildren(node);
        LinkedList firstChild = children ? LinkedList_getNext(children) : NULL;

        if (!firstChild) {
            DataAttributeType* boxed = malloc(sizeof(DataAttributeType));
            if (boxed) {
                *boxed = ((DataAttribute*) node)->type;
                LinkedList_add(result, boxed);
            }
            if (children) LinkedList_destroyStatic(children);
            return;
        }

        LinkedList element = firstChild;
        while (element) {
            collectLeafWireTypesByFc((ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
        return;
    }

    LinkedList children = ModelNode_getChildren(node);
    if (children) {
        LinkedList element = LinkedList_getNext(children);
        while (element) {
            collectLeafWireTypesByFc((ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
    }
}

/*
 * Member-reference-keyed counterpart of IedModelUseCases_getDataSetMemberLeafWireTypes -
 * see IedModelUseCases_getLeafReferencesForMemberReference's own doc comment
 * for why this exists. Index-aligned with that same function's own output
 * for the same memberReference.
 */
LinkedList
IedModelUseCases_getLeafWireTypesForMemberReference(IedModelHandle handle, const char* memberReference) {
    LinkedList result = LinkedList_create();
    ModelNode* doNode = NULL;
    FunctionalConstraint fc = IEC61850_FC_NONE;
    if (!resolveDoLevelMemberReference(handle->model, memberReference, &doNode, &fc)) return result;

    LinkedList doChildren = ModelNode_getChildren(doNode);
    if (doChildren) {
        LinkedList element = LinkedList_getNext(doChildren);
        while (element) {
            collectLeafWireTypesByFc((ModelNode*) LinkedList_getData(element), fc, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(doChildren);
    }
    return result;
}

LinkedList
IedModelUseCases_getDataSetMemberLeafWireTypes(IedModelHandle handle, const char* datasetReference,
        int memberIndex) {
    char* memberRef = resolveDataSetMemberReference(handle, datasetReference, memberIndex);
    if (!memberRef) return LinkedList_create();

    LinkedList result = IedModelUseCases_getLeafWireTypesForMemberReference(handle, memberRef);
    free(memberRef);
    return result;
}

/*
 * Cross-checks one leaf's EXPECTED (SCL-declared) DataAttributeType against
 * its ACTUAL wire-decoded MmsType, before mms_report_client/goose_subscriber
 * trust a Gap-4 decomposition zip. Exists because the pre-existing
 * count-only fallback (see IedModelUseCases_getDataSetMemberLeafReferences's
 * own doc comment on the wire-order assumption) cannot catch a
 * same-count-but-different-ORDER mismatch between this daemon's
 * locally-resolved leaf order (SCL <DOType> XML child order) and a real
 * device's actual runtime attribute order - confirmed against real
 * production hardware: a DPC's "Pos" structured attribute had its stVal/t
 * sub-elements zipped to the wrong reference labels (a UTC_TIME value landed
 * on the "stVal" reference, a BOOLEAN landed on "t") because both orderings
 * happened to have the same leaf count. Only implements CONFIDENT, well-
 * established groupings - per this codebase's "don't guess IEC 61850
 * semantics" rule (same conservative posture IedModelUtils_mapBType itself
 * already takes for an unrecognized bType), anything not explicitly listed
 * below always matches (no check), rather than risk a false-positive
 * rejection of a genuinely well-ordered structure.
 */
bool
IedModelUseCases_dataAttributeTypeMatchesMmsType(DataAttributeType expected, MmsType actual) {
    switch (expected) {
        case IEC61850_BOOLEAN:
            return actual == MMS_BOOLEAN;
        case IEC61850_TIMESTAMP:
            return actual == MMS_UTC_TIME;
        case IEC61850_QUALITY:
        case IEC61850_CODEDENUM:
        case IEC61850_CHECK:
        case IEC61850_GENERIC_BITSTRING:
        case IEC61850_OPTFLDS:
        case IEC61850_TRGOPS:
            return actual == MMS_BIT_STRING;
        case IEC61850_INT8:
        case IEC61850_INT16:
        case IEC61850_INT32:
        case IEC61850_INT64:
        case IEC61850_INT128:
        case IEC61850_INT8U:
        case IEC61850_INT16U:
        case IEC61850_INT24U:
        case IEC61850_INT32U:
        case IEC61850_FLOAT32:
        case IEC61850_FLOAT64:
        case IEC61850_ENUMERATED:
            return actual == MMS_INTEGER || actual == MMS_UNSIGNED || actual == MMS_FLOAT;
        case IEC61850_VISIBLE_STRING_32:
        case IEC61850_VISIBLE_STRING_64:
        case IEC61850_VISIBLE_STRING_65:
        case IEC61850_VISIBLE_STRING_129:
        case IEC61850_VISIBLE_STRING_255:
        case IEC61850_UNICODE_STRING_255:
            return actual == MMS_VISIBLE_STRING || actual == MMS_STRING;
        default:
            /* IEC61850_UNKNOWN_TYPE, IEC61850_OCTET_STRING_*, IEC61850_ENTRY_TIME,
             * IEC61850_PHYCOMADDR, IEC61850_CURRENCY, IEC61850_CONSTRUCTED - not
             * confident enough to assert a specific MmsType, so never reject. */
            return true;
    }
}

/* Walks every direct child (Data Object) of `ln`, collecting "$"-joined leaf
 * references at functional constraint `fc` via collectLeafReferencesByFc -
 * same walk IedModelUseCases_getDataSetMemberLeafReferences uses per-DO, just
 * driven for every DO under the LN instead of one already-known DO. basePath
 * must already include the "LD/LN$FC" prefix (FC placed here, once, rather
 * than re-derived per DataAttribute - collectLeafReferencesByFc itself only
 * ever appends "$<name>" per level). */
static void
collectLnLeavesByFc(LogicalNode* ln, FunctionalConstraint fc, const char* ldName, const char* lnName,
        LinkedList result) {
    const char* fcName = FunctionalConstraint_toString(fc);
    size_t baseLen = strlen(ldName) + 1 + strlen(lnName) + 1 + strlen(fcName) + 1;
    char* basePath = malloc(baseLen);
    if (!basePath) return;
    snprintf(basePath, baseLen, "%s/%s$%s", ldName, lnName, fcName);

    LinkedList children = ModelNode_getChildren((ModelNode*) ln);
    if (children) {
        LinkedList element = LinkedList_getNext(children);
        while (element) {
            collectLeafReferencesByFc((ModelNode*) LinkedList_getData(element), fc, basePath, result);
            element = LinkedList_getNext(element);
        }
        LinkedList_destroyStatic(children);
    }
    free(basePath);
}

/*
 * Deliberately applies NO categoryFilter check of its own, unlike
 * getGooseSubscriptionTargets/getReportSubscriptionTargets/
 * getReportableAttributeReferencesForWholeDevice above/below - this
 * single-LN variant is only ever called (by mms_report_client, via
 * ReportControlBlockTarget.lnReference) with an lnReference that already
 * survived getReportSubscriptionTargets's own filter: an RCB whose parent LN
 * doesn't match the active filter was never returned as a target in the
 * first place, so this function is never reached for it. Filtering here too
 * would be redundant, not wrong - but a future caller must not reach this
 * function with an unfiltered LN and expect filtering to happen here; it
 * won't.
 */
LinkedList
IedModelUseCases_getReportableAttributeReferencesForLogicalNode(IedModelHandle handle, const char* lnReference) {
    LinkedList result = LinkedList_create();
    if (!lnReference) return result;

    char* refCopy = strdup(lnReference);
    if (!refCopy) return result;

    char* slash = strchr(refCopy, '/');
    if (!slash) {
        free(refCopy);
        return result;
    }
    *slash = '\0';
    const char* ldName = refCopy;
    const char* lnName = slash + 1;

    LogicalDevice* ld = IedModel_getDevice(handle->model, ldName);
    LogicalNode* ln = ld ? LogicalDevice_getLogicalNode(ld, lnName) : NULL;
    if (!ln) {
        free(refCopy);
        return result;
    }

    /* "All the variables" = every leaf DA at FC=ST (status) or FC=MX
     * (measurand) under this one LN - the same FC pair getReadTargets already
     * treats as "reportable" elsewhere in this codebase. Output format
     * ("LD/LN$FC$DO$DA") matches getDataSetMemberReferences exactly, so every
     * downstream consumer (reference labeling, ipc_dispatcher's quality
     * pairing) treats this identically to an SCL-declared dataset's members. */
    collectLnLeavesByFc(ln, IEC61850_FC_ST, ldName, lnName, result);
    collectLnLeavesByFc(ln, IEC61850_FC_MX, ldName, lnName, result);

    free(refCopy);
    return result;
}

/* Whole-device counterpart of IedModelUseCases_getReportableAttributeReferencesForLogicalNode -
 * same FC=ST/MX "every leaf" convention, same "LD/LN$FC$DO$DA" output format,
 * but walks EVERY LN under EVERY LD in the model (mirrors IedModelUseCases_getReadTargets'
 * own model.firstChild/sibling walk, ld.firstChild/sibling for LNs, rather than
 * resolving one caller-supplied lnReference). Used by mms_report_client's
 * whole-device dynamic-dataset clustering: a "Dyn" RCB's own parent LN does
 * NOT restrict what a dataset assigned to it can report on (verified: neither
 * IedConnection_createDataSet's wire format nor this codebase's own
 * MmsDatasetManagerUseCases_buildWireMemberReferences ties a dataset member to
 * any particular LN - each member reference is independently addressed), so
 * the daemon can cover the ENTIRE device's reportable data through however
 * many spare RCB "slots" exist anywhere in the model, not just the slots that
 * happen to be parented on the same LN as the data itself. Purely local,
 * never touches the network. Caller owns the list and its elements
 * (LinkedList_destroyDeep(list, free)). */
LinkedList
IedModelUseCases_getReportableAttributeReferencesForWholeDevice(IedModelHandle handle) {
    LinkedList result = LinkedList_create();
    if (!handle || !handle->model) return result;

    ModelNode* ldNode = (ModelNode*) handle->model->firstChild;
    while (ldNode) {
        ModelNode* lnNode = ldNode->firstChild;
        while (lnNode) {
            /* LogicalDevice.name is only the bare LD instance name (e.g.
             * "LD1") - the externally-visible "LD/LN" reference needs the IED
             * name prepended too (or the SCL-declared functional ldName, if
             * present), which is exactly what ModelNode_getObjectReference
             * already computes correctly - same call
             * IedModelUseCases_getReportSubscriptionTargets/
             * _getGooseSubscriptionTargets already use for a parent LN node,
             * reused here rather than re-deriving the IED-name-prefixing rule
             * by hand. */
            LnCategory lnCategory = categoryForLn(handle, (LogicalNode*) lnNode);
            /* alwaysInclude (LLN0) wins over the mask outright - an LD's own
             * status node has to be in every dynamic dataset this plan
             * produces regardless of what the technician selected, or a
             * Control-only connect silently loses Mod/Beh/Health for every
             * LD on the device. */
            if (alwaysIncludeForLn(handle, (LogicalNode*) lnNode) || (handle->categoryFilter & lnCategory)) {
                char* lnRef = ModelNode_getObjectReference(lnNode, NULL);
                if (lnRef) {
                    char* slash = strchr(lnRef, '/');
                    if (slash) {
                        *slash = '\0';
                        collectLnLeavesByFc((LogicalNode*) lnNode, IEC61850_FC_ST, lnRef, slash + 1, result);
                        collectLnLeavesByFc((LogicalNode*) lnNode, IEC61850_FC_MX, lnRef, slash + 1, result);
                    }
                    free(lnRef);
                }
            } else if (handle->categoryFilter != IED_MODEL_LN_CATEGORY_ALL) {
                /* Diagnostic-only, and only once a filter is genuinely active
                 * (skip the noise on the common unfiltered path) - logs what
                 * got excluded and why, since mms_dataset_manager's own
                 * cluster-plan logging only ever sees the already-filtered
                 * survivors and has no LN-category context of its own to
                 * explain an absence. */
                char* lnRef = ModelNode_getObjectReference(lnNode, NULL);
                if (lnRef) {
                    /* Keep the full "LD/LN" reference for the log line -
                     * splitLnRef is a separate, truncated copy used only to
                     * feed collectLnLeavesByFc's own (ldName, lnName) pair. */
                    char* splitLnRef = strdup(lnRef);
                    LinkedList skipped = LinkedList_create();
                    if (splitLnRef) {
                        char* slash = strchr(splitLnRef, '/');
                        if (slash) {
                            *slash = '\0';
                            collectLnLeavesByFc((LogicalNode*) lnNode, IEC61850_FC_ST, splitLnRef, slash + 1,
                                    skipped);
                            collectLnLeavesByFc((LogicalNode*) lnNode, IEC61850_FC_MX, splitLnRef, slash + 1,
                                    skipped);
                        }
                    }
                    char maskStr[64];
                    formatCategoryMask(handle->categoryFilter, maskStr, sizeof(maskStr));
                    fprintf(stderr,
                            "[ied_model] LN '%s' (category %s) excluded by active filter (%s) - "
                            "%d leaf attribute(s) skipped\n",
                            lnRef, IedModelLnCategory_toString(lnCategory), maskStr, LinkedList_size(skipped));
                    LinkedList_destroyDeep(skipped, free);
                    free(splitLnRef);
                    free(lnRef);
                }
            }
            lnNode = lnNode->sibling;
        }
        ldNode = ldNode->sibling;
    }

    return result;
}

LnCategoryMask
IedModelUseCases_getCategoryFilter(IedModelHandle handle) {
    return handle ? handle->categoryFilter : IED_MODEL_LN_CATEGORY_ALL;
}

int
IedModelUseCases_getDynDataSetMax(IedModelHandle handle) {
    return handle ? handle->dynDataSetMax : -1;
}

int
IedModelUseCases_getDynDataSetMaxAttributes(IedModelHandle handle) {
    return handle ? handle->dynDataSetMaxAttributes : -1;
}

int
IedModelUseCases_getConfDataSetMax(IedModelHandle handle) {
    return handle ? handle->confDataSetMax : -1;
}

int
IedModelUseCases_getConfDataSetMaxAttributes(IedModelHandle handle) {
    return handle ? handle->confDataSetMaxAttributes : -1;
}

LinkedList
IedModelUseCases_getReadTargets(IedModelHandle handle) {
    LinkedList result = LinkedList_create();

    ModelNode* ldNode = (ModelNode*) handle->model->firstChild;
    while (ldNode) {
        collectDataAttributesByFc(ldNode, IEC61850_FC_ST, result);
        collectDataAttributesByFc(ldNode, IEC61850_FC_MX, result);
        ldNode = ldNode->sibling;
    }

    return result;
}

LinkedList
IedModelUseCases_getControlTargets(IedModelHandle handle) {
    LinkedList result = LinkedList_create();

    ModelNode* ldNode = (ModelNode*) handle->model->firstChild;
    while (ldNode) {
        collectControllableDataObjects(ldNode, result);
        ldNode = ldNode->sibling;
    }

    return result;
}
