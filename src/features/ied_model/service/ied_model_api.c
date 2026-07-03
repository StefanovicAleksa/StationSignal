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

IedModelHandle
IedModel_loadFromFile(const char* path, const char* iedName, AccessMode mode, IedModelLoadError* outError) {
    IedModelLoadError localError;
    IedModel* model = IedModelSclLoader_load(path, iedName, &localError);

    if (outError) *outError = localError;
    if (!model) return NULL;

    IedModelHandle handle = malloc(sizeof(struct sIedModelHandle));
    if (!handle) {
        IedModel_destroy(model);
        if (outError) *outError = IED_MODEL_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    handle->model = model;
    handle->accessMode = mode;
    handle->iedName = copyString(iedName);

    return handle;
}

void
IedModel_release(IedModelHandle handle) {
    if (!handle) return;
    IedModel_destroy(handle->model);
    free(handle->iedName);
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
