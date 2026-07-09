#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model/domain/ied_model_usecases.h"
#include "iec61850_dynamic_model.h"
#include "iec61850_common.h"

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

/* ---- public use-cases ---- */

LinkedList
IedModelUseCases_getGooseSubscriptionTargets(IedModelHandle handle) {
    LinkedList result = LinkedList_create();

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
        if (!entry->logicalDeviceName || !entry->variableName) continue;
        size_t len = strlen(entry->logicalDeviceName) + 1 + strlen(entry->variableName) + 1;
        char* ref = malloc(len);
        if (ref) {
            snprintf(ref, len, "%s/%s", entry->logicalDeviceName, entry->variableName);
            LinkedList_add(result, ref);
        }
    }
    return result;
}

LinkedList
IedModelUseCases_getDataSetMemberLeafReferences(IedModelHandle handle, const char* datasetReference,
        int memberIndex) {
    LinkedList result = LinkedList_create();
    if (!datasetReference || memberIndex < 0) return result;

    DataSet* dataSet = IedModel_lookupDataSet(handle->model, datasetReference);
    if (!dataSet) return result;

    DataSetEntry* entry = DataSet_getFirstEntry(dataSet);
    for (int i = 0; entry && i < memberIndex; i++) entry = DataSetEntry_getNext(entry);
    if (!entry || !entry->logicalDeviceName || !entry->variableName) return result;

    /* variableName is "$"-joined "LN$FC$DO" (decomposition candidate) or
     * "LN$FC$DO$DA" (already leaf-level) - see
     * IedModelUtils_buildFcdaVariableName's own doc comment. */
    char* variableNameCopy = strdup(entry->variableName);
    if (!variableNameCopy) return result;

    char* lnToken = strtok(variableNameCopy, "$");
    char* fcToken = lnToken ? strtok(NULL, "$") : NULL;
    char* doToken = fcToken ? strtok(NULL, "$") : NULL;
    char* daToken = doToken ? strtok(NULL, "$") : NULL;

    if (!doToken || daToken) {
        /* Already leaf-level, or an unexpectedly-shaped variableName -
         * nothing to decompose either way. */
        free(variableNameCopy);
        return result;
    }

    FunctionalConstraint fc = FunctionalConstraint_fromString(fcToken);
    LogicalDevice* ld = IedModel_getDevice(handle->model, entry->logicalDeviceName);
    LogicalNode* ln = ld ? LogicalDevice_getLogicalNode(ld, lnToken) : NULL;
    ModelNode* doNode = ln ? ModelNode_getChild((ModelNode*) ln, doToken) : NULL;

    if (doNode && ModelNode_getType(doNode) == DataObjectModelType) {
        /* Base "$"-joined path for the DO itself - each child's leaf path is
         * built as basePath + "$" + <child names...> by
         * collectLeafReferencesByFc, matching the same "LD/LN$FC$DO$DA"
         * style entry->variableName already uses. */
        size_t baseLen = strlen(entry->logicalDeviceName) + 1 + strlen(lnToken) + 1 + strlen(fcToken) + 1
                + strlen(doToken) + 1;
        char* basePath = malloc(baseLen);
        if (basePath) {
            snprintf(basePath, baseLen, "%s/%s$%s$%s", entry->logicalDeviceName, lnToken, fcToken, doToken);

            LinkedList doChildren = ModelNode_getChildren(doNode);
            if (doChildren) {
                LinkedList element = LinkedList_getNext(doChildren);
                while (element) {
                    collectLeafReferencesByFc((ModelNode*) LinkedList_getData(element), fc, basePath, result);
                    element = LinkedList_getNext(element);
                }
                LinkedList_destroyStatic(doChildren);
            }
            free(basePath);
        }
    }

    free(variableNameCopy);
    return result;
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
