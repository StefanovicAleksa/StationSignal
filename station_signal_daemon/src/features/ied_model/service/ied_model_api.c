#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model/service/ied_model_api.h"
#include "features/ied_model/data/ied_model_scl_loader.h"
#include "features/ied_model/domain/ied_model_usecases.h"
#include "iec61850_dynamic_model.h"

static char*
copyString(const char* s) {
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/* Copies daSemanticsList (owned by the caller, freed here) into a flat,
 * handle-owned array. Degrades to an empty array (never crashes) on
 * allocation failure - same OOM posture as the rest of this feature. */
static void
adoptDaSemantics(IedModelHandle handle, LinkedList daSemanticsList) {
    handle->daSemantics = NULL;
    handle->daSemanticCount = 0;
    if (!daSemanticsList) return;

    int count = LinkedList_size(daSemanticsList);
    if (count > 0) {
        IedModelDaSemanticEntry* array = malloc(sizeof(IedModelDaSemanticEntry) * (size_t) count);
        if (array) {
            int i = 0;
            for (LinkedList element = LinkedList_getNext(daSemanticsList); element;
                    element = LinkedList_getNext(element)) {
                IedModelDaSemanticEntry* boxed = (IedModelDaSemanticEntry*) LinkedList_getData(element);
                array[i++] = *boxed;
            }
            handle->daSemantics = array;
            handle->daSemanticCount = i;
        }
    }

    LinkedList_destroyDeep(daSemanticsList, free);
}

IedModelHandle
IedModel_loadFromFile(const char* path, const char* iedName, AccessMode mode, IedModelLoadError* outError) {
    IedModelLoadError localError;
    LinkedList daSemanticsList = NULL;
    IedModel* model = IedModelSclLoader_load(path, iedName, &localError, &daSemanticsList);

    if (outError) *outError = localError;
    if (!model) {
        if (daSemanticsList) LinkedList_destroyDeep(daSemanticsList, free);
        return NULL;
    }

    IedModelHandle handle = malloc(sizeof(struct sIedModelHandle));
    if (!handle) {
        IedModel_destroy(model);
        if (daSemanticsList) LinkedList_destroyDeep(daSemanticsList, free);
        if (outError) *outError = IED_MODEL_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    handle->model = model;
    handle->accessMode = mode;
    handle->iedName = copyString(iedName);
    adoptDaSemantics(handle, daSemanticsList);

    return handle;
}

LinkedList
IedModel_listIedNames(const char* path, IedModelLoadError* outError) {
    return IedModelSclLoader_listIedNames(path, outError);
}

IedModelHandle
IedModel_wrapDynamicModel(IedModel* model, const char* iedName, AccessMode mode) {
    if (!model) return NULL;

    IedModelHandle handle = malloc(sizeof(struct sIedModelHandle));
    if (!handle) return NULL;

    handle->model = model;
    handle->accessMode = mode;
    handle->iedName = copyString(iedName ? iedName : "");
    /* No SCL bType is ever available over the wire for a dynamically-built
     * (online-discovered) model - an already-accepted limitation, not a
     * regression. Every accessor degrades to IED_MODEL_DA_SEMANTIC_NONE. */
    handle->daSemantics = NULL;
    handle->daSemanticCount = 0;

    return handle;
}

void
IedModel_release(IedModelHandle handle) {
    if (!handle) return;
    IedModel_destroy(handle->model);
    free(handle->iedName);
    free(handle->daSemantics);
    free(handle);
}

LinkedList
IedModel_getGooseSubscriptionTargets(IedModelHandle handle) {
    return IedModelUseCases_getGooseSubscriptionTargets(handle);
}

LinkedList
IedModel_getReportSubscriptionTargets(IedModelHandle handle) {
    return IedModelUseCases_getReportSubscriptionTargets(handle);
}

LinkedList
IedModel_getDataSetMemberReferences(IedModelHandle handle, const char* datasetReference) {
    return IedModelUseCases_getDataSetMemberReferences(handle, datasetReference);
}

LinkedList
IedModel_getDataSetMemberLeafReferences(IedModelHandle handle, const char* datasetReference, int memberIndex) {
    return IedModelUseCases_getDataSetMemberLeafReferences(handle, datasetReference, memberIndex);
}

LinkedList
IedModel_getDataSetMemberSemantics(IedModelHandle handle, const char* datasetReference) {
    return IedModelUseCases_getDataSetMemberSemantics(handle, datasetReference);
}

LinkedList
IedModel_getDataSetMemberLeafSemantics(IedModelHandle handle, const char* datasetReference, int memberIndex) {
    return IedModelUseCases_getDataSetMemberLeafSemantics(handle, datasetReference, memberIndex);
}

LinkedList
IedModel_getDataSetMemberLeafWireTypes(IedModelHandle handle, const char* datasetReference, int memberIndex) {
    return IedModelUseCases_getDataSetMemberLeafWireTypes(handle, datasetReference, memberIndex);
}

bool
IedModel_dataAttributeTypeMatchesMmsType(DataAttributeType expected, MmsType actual) {
    return IedModelUseCases_dataAttributeTypeMatchesMmsType(expected, actual);
}

LinkedList
IedModel_getReportableAttributeReferencesForLogicalNode(IedModelHandle handle, const char* lnReference) {
    return IedModelUseCases_getReportableAttributeReferencesForLogicalNode(handle, lnReference);
}

LinkedList
IedModel_getLeafReferencesForMemberReference(IedModelHandle handle, const char* memberReference) {
    return IedModelUseCases_getLeafReferencesForMemberReference(handle, memberReference);
}

LinkedList
IedModel_getLeafWireTypesForMemberReference(IedModelHandle handle, const char* memberReference) {
    return IedModelUseCases_getLeafWireTypesForMemberReference(handle, memberReference);
}

LinkedList
IedModel_getLeafSemanticsForMemberReference(IedModelHandle handle, const char* memberReference) {
    return IedModelUseCases_getLeafSemanticsForMemberReference(handle, memberReference);
}

IedModelDaSemantic
IedModel_getSemanticForMemberReference(IedModelHandle handle, const char* memberReference) {
    return IedModelUseCases_getSemanticForMemberReference(handle, memberReference);
}

void
IedModel_destroyReportControlBlockTarget(void* target) {
    IedModelUseCases_destroyReportControlBlockTarget(target);
}

void
IedModel_destroyGooseSubscriptionTarget(void* target) {
    IedModelUseCases_destroyGooseSubscriptionTarget(target);
}

LinkedList
IedModel_getReadTargets(IedModelHandle handle) {
    if (handle->accessMode == IED_MODEL_ACCESS_REPORT_ONLY) {
        fprintf(stderr, "[ied_model] read targets denied: handle for '%s' is REPORT_ONLY\n", handle->iedName);
        return LinkedList_create();
    }
    return IedModelUseCases_getReadTargets(handle);
}

LinkedList
IedModel_getControlTargets(IedModelHandle handle) {
    if (handle->accessMode != IED_MODEL_ACCESS_READ_AND_WRITE) {
        fprintf(stderr, "[ied_model] control targets denied: handle for '%s' is not READ_AND_WRITE\n",
                handle->iedName);
        return LinkedList_create();
    }
    return IedModelUseCases_getControlTargets(handle);
}
