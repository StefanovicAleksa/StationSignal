#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model/service/ied_model_api.h"
#include "features/ied_model/data/ied_model_scl_loader.h"
#include "features/ied_model/domain/ied_model_usecases.h"
#include "features/ied_model/utils/ied_model_ln_category.h"
#include "iec61850_dynamic_model.h"
#include "log.h"

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

/* Same shape as adoptDaSemantics - lnCategoriesList's boxed entries own
 * nothing beyond themselves (`ln` is borrowed), so plain free() destroys them. */
static void
adoptLnCategories(IedModelHandle handle, LinkedList lnCategoriesList) {
    handle->lnCategories = NULL;
    handle->lnCategoryCount = 0;
    if (!lnCategoriesList) return;

    int count = LinkedList_size(lnCategoriesList);
    if (count > 0) {
        IedModelLnCategoryEntry* array = malloc(sizeof(IedModelLnCategoryEntry) * (size_t) count);
        if (array) {
            int i = 0;
            for (LinkedList element = LinkedList_getNext(lnCategoriesList); element;
                    element = LinkedList_getNext(element)) {
                IedModelLnCategoryEntry* boxed = (IedModelLnCategoryEntry*) LinkedList_getData(element);
                array[i++] = *boxed;
            }
            handle->lnCategories = array;
            handle->lnCategoryCount = i;
        }
    }

    LinkedList_destroyDeep(lnCategoriesList, free);
}

/* LinkedListValueDeleteFunction-compatible: frees a boxed IedModelDaDescEntry
 * AND its owned `desc` string - unlike adoptDaSemantics/adoptLnCategories's
 * boxed entries, plain free() here would leak `desc`. */
static void
freeDaDescEntryBox(void* entry) {
    if (!entry) return;
    IedModelDaDescEntry* descEntry = (IedModelDaDescEntry*) entry;
    free(descEntry->desc);
    free(descEntry);
}

/* Same shape as adoptDaSemantics/adoptLnCategories, but each copied array
 * element's `desc` string is itself owned by the handle now (the source
 * LinkedList's boxed entries are destroyed via freeDaDescEntryBox, not plain
 * free, since the box owns `desc` - the array copy below takes over that
 * ownership by value, same as `da` is taken over by reference). */
static void
adoptDaDescriptions(IedModelHandle handle, LinkedList daDescriptionsList) {
    handle->daDescriptions = NULL;
    handle->daDescriptionCount = 0;
    if (!daDescriptionsList) return;

    int count = LinkedList_size(daDescriptionsList);
    if (count > 0) {
        IedModelDaDescEntry* array = malloc(sizeof(IedModelDaDescEntry) * (size_t) count);
        if (array) {
            int i = 0;
            for (LinkedList element = LinkedList_getNext(daDescriptionsList); element;
                    element = LinkedList_getNext(element)) {
                IedModelDaDescEntry* boxed = (IedModelDaDescEntry*) LinkedList_getData(element);
                array[i++] = *boxed; /* takes over ownership of boxed->desc by value */
            }
            handle->daDescriptions = array;
            handle->daDescriptionCount = i;
            /* Ownership of each entry's `desc` string moved into the array by
             * value above (a plain struct-copy also copies the pointer) -
             * destroy only the boxes themselves here (plain free, same as
             * adoptDaSemantics/adoptLnCategories), never freeDaDescEntryBox,
             * which would free `desc` out from under the array's own copy of
             * that same pointer. */
            LinkedList_destroyDeep(daDescriptionsList, free);
            return;
        }
    }

    /* count == 0, or the array allocation failed - no array was built, so
     * ownership of every entry's `desc` never transferred anywhere; free it
     * here (via freeDaDescEntryBox) or it leaks. */
    LinkedList_destroyDeep(daDescriptionsList, freeDaDescEntryBox);
}

IedModelHandle
IedModel_loadFromFile(const char* path, const char* iedName, AccessMode mode, LnCategoryMask categoryFilter,
        IedModelLoadError* outError) {
    IedModelLoadError localError;
    LinkedList daSemanticsList = NULL;
    LinkedList lnCategoriesList = NULL;
    LinkedList daDescriptionsList = NULL;
    int dynDataSetMax = -1;
    int dynDataSetMaxAttributes = -1;
    int confDataSetMax = -1;
    int confDataSetMaxAttributes = -1;
    IedModel* model = IedModelSclLoader_load(path, iedName, &localError, &daSemanticsList, &lnCategoriesList,
            &daDescriptionsList, &dynDataSetMax, &dynDataSetMaxAttributes, &confDataSetMax,
            &confDataSetMaxAttributes);

    if (outError) *outError = localError;
    if (!model) {
        if (daSemanticsList) LinkedList_destroyDeep(daSemanticsList, free);
        if (lnCategoriesList) LinkedList_destroyDeep(lnCategoriesList, free);
        if (daDescriptionsList) LinkedList_destroyDeep(daDescriptionsList, freeDaDescEntryBox);
        return NULL;
    }

    IedModelHandle handle = malloc(sizeof(struct sIedModelHandle));
    if (!handle) {
        IedModel_destroy(model);
        if (daSemanticsList) LinkedList_destroyDeep(daSemanticsList, free);
        if (lnCategoriesList) LinkedList_destroyDeep(lnCategoriesList, free);
        if (daDescriptionsList) LinkedList_destroyDeep(daDescriptionsList, freeDaDescEntryBox);
        if (outError) *outError = IED_MODEL_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    handle->model = model;
    handle->accessMode = mode;
    handle->iedName = copyString(iedName);
    adoptDaSemantics(handle, daSemanticsList);
    adoptLnCategories(handle, lnCategoriesList);
    adoptDaDescriptions(handle, daDescriptionsList);
    handle->dynDataSetMax = dynDataSetMax;
    handle->dynDataSetMaxAttributes = dynDataSetMaxAttributes;
    handle->confDataSetMax = confDataSetMax;
    handle->confDataSetMaxAttributes = confDataSetMaxAttributes;
    handle->categoryFilter = categoryFilter;

    return handle;
}

LinkedList
IedModel_listIedNames(const char* path, IedModelLoadError* outError) {
    return IedModelSclLoader_listIedNames(path, outError);
}

IedModelHandle
IedModel_wrapDynamicModel(IedModel* model, const char* iedName, AccessMode mode, LinkedList lnCategoriesList,
        LnCategoryMask categoryFilter) {
    if (!model) {
        if (lnCategoriesList) LinkedList_destroyDeep(lnCategoriesList, free);
        return NULL;
    }

    IedModelHandle handle = malloc(sizeof(struct sIedModelHandle));
    if (!handle) {
        if (lnCategoriesList) LinkedList_destroyDeep(lnCategoriesList, free);
        return NULL;
    }

    handle->model = model;
    handle->accessMode = mode;
    handle->iedName = copyString(iedName ? iedName : "");
    /* No SCL bType is ever available over the wire for a dynamically-built
     * (online-discovered) model - an already-accepted limitation, not a
     * regression. Every accessor degrades to IED_MODEL_DA_SEMANTIC_NONE. */
    handle->daSemantics = NULL;
    handle->daSemanticCount = 0;
    /* No <Services> is ever available on a dynamically-built (online-discovered)
     * model - same "unknown" posture as daSemantics above. */
    handle->dynDataSetMax = -1;
    handle->dynDataSetMaxAttributes = -1;
    handle->confDataSetMax = -1;
    handle->confDataSetMaxAttributes = -1;
    /* Unlike daSemantics, NOT left permanently empty for a wrapped model -
     * the caller (ied_model_online_loader) classifies each LN it discovers
     * via IedModel_categorizeWireInstanceName as it builds them, since
     * leaving this empty would make the whole category filter inert on the
     * online-discovery path. adoptLnCategories degrades to an empty array
     * (same as every other adopt* function) if lnCategoriesList is NULL. */
    adoptLnCategories(handle, lnCategoriesList);
    /* No SCL desc="..." equivalent is ever available over MMS ACSI directory
     * services - always empty on this path, same "unknown" posture as
     * daSemantics/dynDataSetMax above (unlike lnCategories, genuinely never
     * populated later either). */
    handle->daDescriptions = NULL;
    handle->daDescriptionCount = 0;
    handle->categoryFilter = categoryFilter;

    return handle;
}

void
IedModel_release(IedModelHandle handle) {
    if (!handle) return;
    IedModel_destroy(handle->model);
    free(handle->iedName);
    free(handle->daSemantics);
    free(handle->lnCategories);
    for (int i = 0; i < handle->daDescriptionCount; i++) free(handle->daDescriptions[i].desc);
    free(handle->daDescriptions);
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
IedModel_getReportableAttributeReferencesForWholeDevice(IedModelHandle handle) {
    return IedModelUseCases_getReportableAttributeReferencesForWholeDevice(handle);
}

LnCategoryMask
IedModel_getCategoryFilter(IedModelHandle handle) {
    return IedModelUseCases_getCategoryFilter(handle);
}

int
IedModel_getDynDataSetMax(IedModelHandle handle) {
    return IedModelUseCases_getDynDataSetMax(handle);
}

int
IedModel_getDynDataSetMaxAttributes(IedModelHandle handle) {
    return IedModelUseCases_getDynDataSetMaxAttributes(handle);
}

int
IedModel_getConfDataSetMax(IedModelHandle handle) {
    return IedModelUseCases_getConfDataSetMax(handle);
}

int
IedModel_getConfDataSetMaxAttributes(IedModelHandle handle) {
    return IedModelUseCases_getConfDataSetMaxAttributes(handle);
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

LnCategory
IedModel_getCategoryForMemberReference(IedModelHandle handle, const char* memberReference) {
    return IedModelUseCases_getCategoryForMemberReference(handle, memberReference);
}

bool
IedModel_isMemberReferenceAlwaysIncluded(IedModelHandle handle, const char* memberReference) {
    return IedModelUseCases_isMemberReferenceAlwaysIncluded(handle, memberReference);
}

const char*
IedModel_getDescriptionForMemberReference(IedModelHandle handle, const char* memberReference) {
    return IedModelUseCases_getDescriptionForMemberReference(handle, memberReference);
}

LinkedList
IedModel_getLeafDescriptionsForMemberReference(IedModelHandle handle, const char* memberReference) {
    return IedModelUseCases_getLeafDescriptionsForMemberReference(handle, memberReference);
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
        SS_LOG_WARN("[ied_model] read targets denied: handle for '%s' is REPORT_ONLY\n", handle->iedName);
        return LinkedList_create();
    }
    return IedModelUseCases_getReadTargets(handle);
}

LinkedList
IedModel_getControlTargets(IedModelHandle handle) {
    if (handle->accessMode != IED_MODEL_ACCESS_READ_AND_WRITE) {
        SS_LOG_WARN("[ied_model] control targets denied: handle for '%s' is not READ_AND_WRITE\n",
                handle->iedName);
        return LinkedList_create();
    }
    return IedModelUseCases_getControlTargets(handle);
}

LnCategory
IedModel_categorizeWireInstanceName(const char* wireName) {
    return IedModelLnCategory_forWireInstanceName(wireName);
}

bool
IedModel_isAlwaysIncludedWireInstanceName(const char* wireName) {
    return IedModelLnCategory_isAlwaysIncludedWireInstanceName(wireName);
}

const char*
IedModel_categoryToString(LnCategory category) {
    return IedModelLnCategory_toString(category);
}
