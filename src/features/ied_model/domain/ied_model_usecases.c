#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model/domain/ied_model_usecases.h"

/* ---- recursive tree walkers over the built model ---- */

static void
collectDataAttributesByFc(ModelNode* node, FunctionalConstraint fc, LinkedList result) {
    if (ModelNode_getType(node) == DataAttributeModelType) {
        if (((DataAttribute*) node)->fc == fc) {
            char* ref = ModelNode_getObjectReference(node, NULL);
            if (ref) LinkedList_add(result, ref);
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
            if (ref) LinkedList_add(result, ref);
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

/* ---- public use-cases ---- */

LinkedList
IedModelUseCases_getGooseSubscriptionTargets(IedModelHandle handle) {
    LinkedList result = LinkedList_create();

    for (GSEControlBlock* gcb = handle->model->gseCBs; gcb; gcb = gcb->sibling) {
        char* lnRef = ModelNode_getObjectReference((ModelNode*) gcb->parent, NULL);
        if (!lnRef) continue;

        /* GoCB reference notation uses "$"-separated MMS style, per goose_subscriber.h's
         * documented example ("simpleIOGenericIO/LLN0$GO$gcbEvents"). */
        size_t len = strlen(lnRef) + strlen("$GO$") + strlen(gcb->name) + 1;
        char* ref = malloc(len);
        if (!ref) {
            free(lnRef);
            continue;
        }
        snprintf(ref, len, "%s$GO$%s", lnRef, gcb->name);
        free(lnRef);

        GooseSubscriptionTarget* target = calloc(1, sizeof(GooseSubscriptionTarget));
        if (!target) {
            free(ref);
            continue;
        }
        target->objectReference = ref;

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
    free(gooseTarget);
}

LinkedList
IedModelUseCases_getReportSubscriptionTargets(IedModelHandle handle) {
    LinkedList result = LinkedList_create();

    for (ReportControlBlock* rcb = handle->model->rcbs; rcb; rcb = rcb->sibling) {
        char* lnRef = ModelNode_getObjectReference((ModelNode*) rcb->parent, NULL);
        if (!lnRef) continue;

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
        free(lnRef);

        if (!ref) {
            free(datasetRef);
            continue;
        }

        ReportControlBlockTarget* target = malloc(sizeof(ReportControlBlockTarget));
        if (!target) {
            free(ref);
            free(datasetRef);
            continue;
        }
        target->objectReference = ref;
        target->buffered = rcb->buffered;
        target->datasetReference = datasetRef;

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
    free(rcbTarget);
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
