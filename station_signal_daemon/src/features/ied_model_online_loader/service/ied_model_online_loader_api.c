#include "features/ied_model_online_loader/service/ied_model_online_loader_api.h"
#include "features/ied_model_online_loader/data/ied_model_online_loader_connection.h"
#include "iec61850_dynamic_model.h"

IedModelHandle
IedModelOnlineLoader_build(const char* host, int port, const char* iedName, AccessMode mode,
        LnCategoryMask categoryFilter, const char* acseAuthPassword, const IedModelOnlineLoaderConfig* config,
        IedModelOnlineLoaderError* outError) {
    IedModelOnlineLoaderError localError;
    LinkedList lnCategoriesList = NULL;
    IedModel* model = IedModelOnlineLoaderConnection_connectAndBuild(host, port, iedName, acseAuthPassword,
            config, &lnCategoriesList, &localError);

    if (outError) *outError = localError;
    if (!model) {
        if (lnCategoriesList) LinkedList_destroyDeep(lnCategoriesList, free);
        return NULL;
    }

    IedModelHandle handle = IedModel_wrapDynamicModel(model, iedName, mode, lnCategoriesList, categoryFilter);
    if (!handle) {
        IedModel_destroy(model);
        if (outError) *outError = IED_MODEL_ONLINE_LOADER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    return handle;
}
