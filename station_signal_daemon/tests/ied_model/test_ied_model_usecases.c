#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ied_model/domain/ied_model_usecases.h"
#include "iec61850_dynamic_model.h"

static IedModel* fixtureModel;
static struct sIedModelHandle fixtureHandle;
static IedModelHandle handle;

/*
 * Tiny in-memory IedModel, built directly via the dynamic model API (no file
 * I/O, no SCL parsing - that's the loader's job and is covered by the E2E
 * test in integration_tests/ instead). Shape:
 *   TestIED / LD1 / LLN0
 *     Mod        (DO, no CO child - must NOT be a control target)
 *       stVal    (BOOLEAN, FC=ST - IS a read target)
 *       ctlModel (ENUMERATED, FC=CF - must NOT be a read target)
 *     TotW       (DO, no CO child)
 *       mag      (FLOAT32, FC=MX - IS a read target)
 *     CSWI       (DO, HAS a CO child - IS a control target)
 *       Oper     (BOOLEAN, FC=CO)
 *     DataSet "events", one ReportControlBlock, one GSEControlBlock
 */
static IedModel*
buildFixtureModel(void) {
    IedModel* model = IedModel_create("TestIED");
    LogicalDevice* ld = LogicalDevice_create("LD1", model);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    DataObject* mod = DataObject_create("Mod", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) mod, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("ctlModel", (ModelNode*) mod, IEC61850_ENUMERATED, IEC61850_FC_CF, 0, 0, 0);

    DataObject* totW = DataObject_create("TotW", (ModelNode*) ln0, 0);
    DataAttribute_create("mag", (ModelNode*) totW, IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);

    DataObject* cswi = DataObject_create("CSWI", (ModelNode*) ln0, 0);
    DataAttribute_create("Oper", (ModelNode*) cswi, IEC61850_BOOLEAN, IEC61850_FC_CO, 0, 0, 0);

    DataSet* dataSet = DataSet_create("events", ln0);
    DataSetEntry_create(dataSet, "TestIEDLD1/LLN0$ST$Mod$stVal", -1, NULL);

    ReportControlBlock_create("brcb01", ln0, "rpt01", true, "events", 1, TRG_OPT_DATA_CHANGED, RPT_OPT_SEQ_NUM, 0, 0);
    GSEControlBlock_create("gcb01", ln0, "1000", "events", 1, false, -1, -1);

    return model;
}

void
setUp(void) {
    fixtureModel = buildFixtureModel();
    fixtureHandle.model = fixtureModel;
    fixtureHandle.accessMode = IED_MODEL_ACCESS_READ_AND_WRITE; /* unused by usecases; gating lives in api.c */
    fixtureHandle.iedName = "TestIED";
    fixtureHandle.categoryFilter = IED_MODEL_LN_CATEGORY_ALL;
    fixtureHandle.lnCategories = NULL;
    fixtureHandle.lnCategoryCount = 0;
    handle = &fixtureHandle;
}

void
tearDown(void) {
    IedModel_destroy(fixtureModel);
}

static const char*
firstElement(LinkedList list) {
    LinkedList element = LinkedList_getNext(list);
    return element ? (const char*) LinkedList_getData(element) : NULL;
}

/* ---- GOOSE subscription targets ---- */

void
test_getGooseSubscriptionTargets_returnsCorrectReference(void) {
    LinkedList targets = IedModelUseCases_getGooseSubscriptionTargets(handle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(targets));

    GooseSubscriptionTarget* target =
        (GooseSubscriptionTarget*) LinkedList_getData(LinkedList_getNext(targets));
    TEST_ASSERT_EQUAL_STRING("TestIEDLD1/LLN0$GO$gcb01", target->objectReference);
    TEST_ASSERT_EQUAL_STRING("TestIEDLD1/LLN0$events", target->datasetReference);
    /* fixture's gcb01 has no GSEControlBlock_addPhyComAddress call - no SCL
     * <Communication> entry was ever attached. */
    TEST_ASSERT_FALSE(target->hasAddress);

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyGooseSubscriptionTarget);
}

void
test_getGooseSubscriptionTargets_empty_whenModelHasNoGseControlBlocks(void) {
    IedModel* bareModel = IedModel_create("Bare");
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getGooseSubscriptionTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(targets));

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyGooseSubscriptionTarget);
    IedModel_destroy(bareModel);
}

void
test_getGooseSubscriptionTargets_populatesAddress_whenPhyComAddressPresent(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    DataSet* dataSet = DataSet_create("ds1", ln0);
    DataSetEntry_create(dataSet, "BareLD1/LLN0$ST$Mod$stVal", -1, NULL);

    GSEControlBlock* gcb = GSEControlBlock_create("gcbAddr", ln0, "1000", "ds1", 1, false, -1, -1);
    uint8_t mac[6] = { 0x01, 0x0c, 0xcd, 0x01, 0x00, 0x05 };
    GSEControlBlock_addPhyComAddress(gcb, PhyComAddress_create(4, 10, 2000, mac));

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getGooseSubscriptionTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(targets));
    GooseSubscriptionTarget* target =
        (GooseSubscriptionTarget*) LinkedList_getData(LinkedList_getNext(targets));
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$GO$gcbAddr", target->objectReference);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ds1", target->datasetReference);
    TEST_ASSERT_TRUE(target->hasAddress);
    TEST_ASSERT_EQUAL_UINT16(10, target->vlanId);
    TEST_ASSERT_EQUAL_UINT8(4, target->vlanPriority);
    TEST_ASSERT_EQUAL_UINT16(2000, target->appId);
    TEST_ASSERT_EQUAL_MEMORY(mac, target->dstMac, 6);

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyGooseSubscriptionTarget);
    IedModel_destroy(bareModel);
}

void
test_getGooseSubscriptionTargets_datasetReferenceNull_whenGcbHasNoDataset(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    GSEControlBlock_create("gcbNoDs", ln0, "1000", NULL, 1, false, -1, -1);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getGooseSubscriptionTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(targets));
    GooseSubscriptionTarget* target =
        (GooseSubscriptionTarget*) LinkedList_getData(LinkedList_getNext(targets));
    TEST_ASSERT_NULL(target->datasetReference);

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyGooseSubscriptionTarget);
    IedModel_destroy(bareModel);
}

/* ---- Report subscription targets ---- */

void
test_getReportSubscriptionTargets_returnsCorrectReference(void) {
    LinkedList targets = IedModelUseCases_getReportSubscriptionTargets(handle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(targets));

    ReportControlBlockTarget* target =
        (ReportControlBlockTarget*) LinkedList_getData(LinkedList_getNext(targets));
    /* fixture's brcb01 is buffered=true on dataset "events" - must use ".BR." */
    TEST_ASSERT_EQUAL_STRING("TestIEDLD1/LLN0.BR.brcb01", target->objectReference);
    TEST_ASSERT_TRUE(target->buffered);
    TEST_ASSERT_EQUAL_STRING("TestIEDLD1/LLN0$events", target->datasetReference);
    TEST_ASSERT_EQUAL_STRING("TestIEDLD1/LLN0", target->lnReference);

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyReportControlBlockTarget);
}

void
test_getReportSubscriptionTargets_unbufferedRcb_usesRpSegment(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    DataSet* dataSet = DataSet_create("ds1", ln0);
    DataSetEntry_create(dataSet, "BareLD1/LLN0$ST$Mod$stVal", -1, NULL);
    ReportControlBlock_create("urcb01", ln0, "rpt02", false, "ds1", 1, TRG_OPT_DATA_CHANGED, RPT_OPT_SEQ_NUM, 0, 0);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getReportSubscriptionTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(targets));
    ReportControlBlockTarget* target =
        (ReportControlBlockTarget*) LinkedList_getData(LinkedList_getNext(targets));
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0.RP.urcb01", target->objectReference);
    TEST_ASSERT_FALSE(target->buffered);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ds1", target->datasetReference);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0", target->lnReference);

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyReportControlBlockTarget);
    IedModel_destroy(bareModel);
}

void
test_getReportSubscriptionTargets_empty_whenModelHasNoRcbs(void) {
    IedModel* bareModel = IedModel_create("Bare");
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getReportSubscriptionTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(targets));

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyReportControlBlockTarget);
    IedModel_destroy(bareModel);
}

void
test_getReportSubscriptionTargets_datasetReferenceNull_whenRcbHasNoDataset(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    ReportControlBlock_create("brcbNoDs", ln0, "rptNoDs", true, NULL, 1, TRG_OPT_DATA_CHANGED, RPT_OPT_SEQ_NUM, 0, 0);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getReportSubscriptionTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(targets));
    ReportControlBlockTarget* target =
        (ReportControlBlockTarget*) LinkedList_getData(LinkedList_getNext(targets));
    TEST_ASSERT_NULL(target->datasetReference);

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyReportControlBlockTarget);
    IedModel_destroy(bareModel);
}

/* ---- Data set member references ---- */

void
test_getDataSetMemberReferences_returnsSingleEntry_forEventsDataset(void) {
    LinkedList refs = IedModelUseCases_getDataSetMemberReferences(handle, "TestIEDLD1/LLN0$events");

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(refs));
    TEST_ASSERT_EQUAL_STRING("TestIEDLD1/LLN0$ST$Mod$stVal", firstElement(refs));

    LinkedList_destroyDeep(refs, free);
}

void
test_getDataSetMemberReferences_preservesOrder_forMultiEntryDataset(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    DataSet* dataSet = DataSet_create("ds1", ln0);
    DataSetEntry_create(dataSet, "BareLD1/LLN0$ST$Mod$stVal", -1, NULL);
    DataSetEntry_create(dataSet, "BareLD1/LLN0$ST$Mod$q", -1, NULL);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList refs = IedModelUseCases_getDataSetMemberReferences(&bareHandle, "BareLD1/LLN0$ds1");

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(refs));
    LinkedList element = LinkedList_getNext(refs);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Mod$stVal", (const char*) LinkedList_getData(element));
    element = LinkedList_getNext(element);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Mod$q", (const char*) LinkedList_getData(element));

    LinkedList_destroyDeep(refs, free);
    IedModel_destroy(bareModel);
}

void
test_getDataSetMemberReferences_empty_whenDatasetReferenceIsNull(void) {
    LinkedList refs = IedModelUseCases_getDataSetMemberReferences(handle, NULL);

    TEST_ASSERT_NOT_NULL(refs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(refs));

    LinkedList_destroyDeep(refs, free);
}

void
test_getDataSetMemberReferences_empty_whenDatasetReferenceDoesNotResolve(void) {
    LinkedList refs = IedModelUseCases_getDataSetMemberReferences(handle, "TestIEDLD1/LLN0$doesNotExist");

    TEST_ASSERT_NOT_NULL(refs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(refs));

    LinkedList_destroyDeep(refs, free);
}

/* ---- Data set member LEAF references (DO-level FCDA decomposition) ---- */

void
test_getDataSetMemberLeafReferences_empty_whenMemberIsAlreadyLeafLevel(void) {
    /* "events" dataset's only entry already has a daName ($stVal) - nothing to decompose. */
    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafReferences(handle, "TestIEDLD1/LLN0$events", 0);

    TEST_ASSERT_NOT_NULL(leaves);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(leaves));

    LinkedList_destroyDeep(leaves, free);
}

void
test_getDataSetMemberLeafReferences_decomposesFlatDo(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    DataObject* ind = DataObject_create("Ind", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) ind, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) ind, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);

    DataSet* dataSet = DataSet_create("ds1", ln0);
    /* No daName - a DO-level FCDA, matching the real ABB REC650 SCD's
     * <FCDA doName="..." fc="ST" /> shape (no daName attribute). */
    DataSetEntry_create(dataSet, "BareLD1/LLN0$ST$Ind", -1, NULL);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafReferences(&bareHandle, "BareLD1/LLN0$ds1", 0);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(leaves));
    LinkedList element = LinkedList_getNext(leaves);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Ind$stVal", (const char*) LinkedList_getData(element));
    element = LinkedList_getNext(element);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Ind$q", (const char*) LinkedList_getData(element));

    LinkedList_destroyDeep(leaves, free);
    IedModel_destroy(bareModel);
}

void
test_getDataSetMemberLeafReferences_recursesIntoConstructedAttribute(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    /* Mirrors the real ABB REC650 WYE->CMV->Vector nesting at a smaller
     * scale: PhV (DO) -> cVal (CONSTRUCTED DA, fc=MX) -> mag (BDA, fc=MX) -
     * "mag" is only reachable by recursing INTO cVal, unlike
     * collectDataAttributesByFc's read-target walk which would terminal-ize
     * at cVal itself. */
    DataObject* phV = DataObject_create("PhV", (ModelNode*) ln0, 0);
    DataAttribute* cVal = DataAttribute_create("cVal", (ModelNode*) phV, IEC61850_CONSTRUCTED, IEC61850_FC_MX, 0, 0, 0);
    DataAttribute_create("mag", (ModelNode*) cVal, IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) phV, IEC61850_QUALITY, IEC61850_FC_MX, 0, 0, 0);

    DataSet* dataSet = DataSet_create("ds1", ln0);
    DataSetEntry_create(dataSet, "BareLD1/LLN0$MX$PhV", -1, NULL);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafReferences(&bareHandle, "BareLD1/LLN0$ds1", 0);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(leaves));
    LinkedList element = LinkedList_getNext(leaves);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$MX$PhV$cVal$mag", (const char*) LinkedList_getData(element));
    element = LinkedList_getNext(element);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$MX$PhV$q", (const char*) LinkedList_getData(element));

    LinkedList_destroyDeep(leaves, free);
    IedModel_destroy(bareModel);
}

void
test_getDataSetMemberLeafReferences_filtersByFc_excludesOtherFcSiblings(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    /* A DPC-shaped DO: stVal/q at ST, Oper at CO - the ST-scoped FCDA below
     * must decompose to stVal/q only, never Oper. */
    DataObject* pos = DataObject_create("Pos", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) pos, IEC61850_INT32U, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) pos, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("Oper", (ModelNode*) pos, IEC61850_BOOLEAN, IEC61850_FC_CO, 0, 0, 0);

    DataSet* dataSet = DataSet_create("ds1", ln0);
    DataSetEntry_create(dataSet, "BareLD1/LLN0$ST$Pos", -1, NULL);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafReferences(&bareHandle, "BareLD1/LLN0$ds1", 0);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(leaves));
    LinkedList element = LinkedList_getNext(leaves);
    while (element) {
        const char* ref = (const char*) LinkedList_getData(element);
        TEST_ASSERT_NULL_MESSAGE(strstr(ref, "Oper"), "CO-scoped Oper must never appear in an ST-scoped decomposition");
        element = LinkedList_getNext(element);
    }

    LinkedList_destroyDeep(leaves, free);
    IedModel_destroy(bareModel);
}

void
test_getDataSetMemberLeafReferences_empty_whenDatasetReferenceIsNull(void) {
    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafReferences(handle, NULL, 0);

    TEST_ASSERT_NOT_NULL(leaves);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(leaves));

    LinkedList_destroyDeep(leaves, free);
}

void
test_getDataSetMemberLeafReferences_empty_whenIndexOutOfRange(void) {
    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafReferences(handle, "TestIEDLD1/LLN0$events", 5);

    TEST_ASSERT_NOT_NULL(leaves);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(leaves));

    LinkedList_destroyDeep(leaves, free);
}

void
test_getDataSetMemberLeafReferences_empty_whenIndexNegative(void) {
    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafReferences(handle, "TestIEDLD1/LLN0$events", -1);

    TEST_ASSERT_NOT_NULL(leaves);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(leaves));

    LinkedList_destroyDeep(leaves, free);
}

/* ---- getLeafReferencesForMemberReference / getLeafWireTypesForMemberReference /
 * getLeafSemanticsForMemberReference / getSemanticForMemberReference (the
 * member-reference-string-keyed counterparts, added so mms_report_client can
 * resolve Gap-4 decomposition/semantics for a dataset pulled live over the
 * wire - one with no DataSet object registered in this IedModel at all,
 * since SCL never declared it. The four pre-existing DataSet-indexed
 * accessors above are now thin wrappers delegating to these.) ---- */

void
test_getLeafReferencesForMemberReference_decomposesFlatDo_withNoDataSetRegisteredAtAll(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    DataObject* ind = DataObject_create("Ind", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) ind, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) ind, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);

    /* Deliberately no DataSet_create/DataSetEntry_create anywhere in this
     * model - proves resolution works purely from the member-reference
     * string, unlike the DataSet-indexed accessor this mirrors
     * (test_getDataSetMemberLeafReferences_decomposesFlatDo), which requires
     * a real registered DataSet to look up. */
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList leaves = IedModelUseCases_getLeafReferencesForMemberReference(&bareHandle, "BareLD1/LLN0$ST$Ind");

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(leaves));
    LinkedList element = LinkedList_getNext(leaves);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Ind$stVal", (const char*) LinkedList_getData(element));
    element = LinkedList_getNext(element);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Ind$q", (const char*) LinkedList_getData(element));

    LinkedList_destroyDeep(leaves, free);
    IedModel_destroy(bareModel);
}

void
test_getLeafReferencesForMemberReference_matchesDataSetIndexedAccessor_forSameDo(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    DataObject* pos = DataObject_create("Pos", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) pos, IEC61850_INT32U, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) pos, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);

    DataSet* dataSet = DataSet_create("ds1", ln0);
    DataSetEntry_create(dataSet, "BareLD1/LLN0$ST$Pos", -1, NULL);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    /* Regression proof: the DataSet-indexed accessor (now a thin wrapper)
     * and the member-reference-keyed one it delegates to must agree exactly
     * for the SAME underlying DO, since the shared resolver refactor must
     * not have changed either's observable behavior. */
    LinkedList viaDataSet = IedModelUseCases_getDataSetMemberLeafReferences(&bareHandle, "BareLD1/LLN0$ds1", 0);
    LinkedList viaMemberRef = IedModelUseCases_getLeafReferencesForMemberReference(&bareHandle, "BareLD1/LLN0$ST$Pos");

    TEST_ASSERT_EQUAL_INT(LinkedList_size(viaDataSet), LinkedList_size(viaMemberRef));
    LinkedList a = LinkedList_getNext(viaDataSet);
    LinkedList b = LinkedList_getNext(viaMemberRef);
    while (a && b) {
        TEST_ASSERT_EQUAL_STRING((const char*) LinkedList_getData(a), (const char*) LinkedList_getData(b));
        a = LinkedList_getNext(a);
        b = LinkedList_getNext(b);
    }

    LinkedList_destroyDeep(viaDataSet, free);
    LinkedList_destroyDeep(viaMemberRef, free);
    IedModel_destroy(bareModel);
}

void
test_getLeafReferencesForMemberReference_empty_whenAlreadyLeafLevel(void) {
    LinkedList leaves = IedModelUseCases_getLeafReferencesForMemberReference(handle,
            "TestIEDLD1/LLN0$ST$Mod$stVal");

    TEST_ASSERT_NOT_NULL(leaves);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(leaves));

    LinkedList_destroyDeep(leaves, free);
}

void
test_getLeafReferencesForMemberReference_empty_whenNull(void) {
    LinkedList leaves = IedModelUseCases_getLeafReferencesForMemberReference(handle, NULL);

    TEST_ASSERT_NOT_NULL(leaves);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(leaves));

    LinkedList_destroyDeep(leaves, free);
}

void
test_getLeafWireTypesForMemberReference_matchesLeafReferences_countAndOrder(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    DataObject* ind = DataObject_create("Ind", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) ind, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) ind, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList wireTypes = IedModelUseCases_getLeafWireTypesForMemberReference(&bareHandle, "BareLD1/LLN0$ST$Ind");

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(wireTypes));
    LinkedList element = LinkedList_getNext(wireTypes);
    TEST_ASSERT_EQUAL_INT(IEC61850_BOOLEAN, *(DataAttributeType*) LinkedList_getData(element));
    element = LinkedList_getNext(element);
    TEST_ASSERT_EQUAL_INT(IEC61850_QUALITY, *(DataAttributeType*) LinkedList_getData(element));

    LinkedList_destroyDeep(wireTypes, free);
    IedModel_destroy(bareModel);
}

void
test_getSemanticForMemberReference_none_forOrdinaryLeaf(void) {
    /* No daSemantics registered on `handle`'s fixture (empty/NULL array) -
     * every leaf degrades to NONE, same as the DataSet-indexed accessor's
     * own degrade-safely posture. */
    IedModelDaSemantic semantic =
            IedModelUseCases_getSemanticForMemberReference(handle, "TestIEDLD1/LLN0$ST$Mod$stVal");
    TEST_ASSERT_EQUAL_INT(IED_MODEL_DA_SEMANTIC_NONE, semantic);
}

void
test_getSemanticForMemberReference_none_whenDoLevel_notLeaf(void) {
    IedModelDaSemantic semantic = IedModelUseCases_getSemanticForMemberReference(handle, "TestIEDLD1/LLN0$ST$Mod");
    TEST_ASSERT_EQUAL_INT(IED_MODEL_DA_SEMANTIC_NONE, semantic);
}

void
test_getSemanticForMemberReference_none_whenNull(void) {
    TEST_ASSERT_EQUAL_INT(IED_MODEL_DA_SEMANTIC_NONE, IedModelUseCases_getSemanticForMemberReference(handle, NULL));
}

/* ---- getCategoryForMemberReference / getDescriptionForMemberReference /
 * getLeafDescriptionsForMemberReference ---- */

void
test_getCategoryForMemberReference_resolvesLnCategory_regardlessOfLeaf(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* xcbrLn = LogicalNode_create("XCBR1", ld);
    DataObject* pos = DataObject_create("Pos", (ModelNode*) xcbrLn, 0);
    DataAttribute_create("stVal", (ModelNode*) pos, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) pos, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);

    IedModelLnCategoryEntry categories[] = { { .ln = xcbrLn, .category = IED_MODEL_LN_CATEGORY_CONTROL } };
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_ALL, .lnCategories = categories, .lnCategoryCount = 1,
        .iedName = "Bare" };

    /* Both leaves under the same LN resolve to the same category - and so
     * does the bare DO-level reference, since category never needs to reach
     * a terminal DataAttribute at all. */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL,
            IedModelUseCases_getCategoryForMemberReference(&bareHandle, "BareLD1/XCBR1$ST$Pos$stVal"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL,
            IedModelUseCases_getCategoryForMemberReference(&bareHandle, "BareLD1/XCBR1$ST$Pos$q"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL,
            IedModelUseCases_getCategoryForMemberReference(&bareHandle, "BareLD1/XCBR1$ST$Pos"));

    IedModel_destroy(bareModel);
}

void
test_getCategoryForMemberReference_other_whenLnDoesNotResolveOrNull(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER,
            IedModelUseCases_getCategoryForMemberReference(handle, "TestIEDLD1/NoSuchLn$ST$Mod$stVal"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelUseCases_getCategoryForMemberReference(handle, NULL));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelUseCases_getCategoryForMemberReference(NULL, NULL));
}

void
test_getDescriptionForMemberReference_returnsCapturedDesc_forMatchingDa(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    DataObject* pos = DataObject_create("Pos", (ModelNode*) ln0, 0);
    DataAttribute* stVal = DataAttribute_create("stVal", (ModelNode*) pos, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) pos, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);

    IedModelDaDescEntry descriptions[] = { { .da = stVal, .desc = (char*) "Circuit breaker position" } };
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_ALL, .daDescriptions = descriptions, .daDescriptionCount = 1,
        .iedName = "Bare" };

    TEST_ASSERT_EQUAL_STRING("Circuit breaker position",
            IedModelUseCases_getDescriptionForMemberReference(&bareHandle, "BareLD1/LLN0$ST$Pos$stVal"));
    /* The sibling leaf has no captured desc entry - must not fall back to
     * stVal's, must not crash. */
    TEST_ASSERT_NULL(IedModelUseCases_getDescriptionForMemberReference(&bareHandle, "BareLD1/LLN0$ST$Pos$q"));

    IedModel_destroy(bareModel);
}

void
test_getDescriptionForMemberReference_null_whenNoDescCapturedOrUnresolvedOrNull(void) {
    TEST_ASSERT_NULL(IedModelUseCases_getDescriptionForMemberReference(handle, "TestIEDLD1/LLN0$ST$Mod$stVal"));
    TEST_ASSERT_NULL(IedModelUseCases_getDescriptionForMemberReference(handle, "TestIEDLD1/NoSuchLn$ST$Mod$stVal"));
    TEST_ASSERT_NULL(IedModelUseCases_getDescriptionForMemberReference(handle, NULL));
}

void
test_getLeafDescriptionsForMemberReference_indexAlignedWithLeafReferences_nullForUncaptured(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    DataObject* ind = DataObject_create("Ind", (ModelNode*) ln0, 0);
    DataAttribute* stVal = DataAttribute_create("stVal", (ModelNode*) ind, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) ind, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);

    IedModelDaDescEntry descriptions[] = { { .da = stVal, .desc = (char*) "Indication status" } };
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_ALL, .daDescriptions = descriptions, .daDescriptionCount = 1,
        .iedName = "Bare" };

    LinkedList refs = IedModelUseCases_getLeafReferencesForMemberReference(&bareHandle, "BareLD1/LLN0$ST$Ind");
    LinkedList descs = IedModelUseCases_getLeafDescriptionsForMemberReference(&bareHandle, "BareLD1/LLN0$ST$Ind");

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(refs));
    TEST_ASSERT_EQUAL_INT(LinkedList_size(refs), LinkedList_size(descs));

    LinkedList refElement = LinkedList_getNext(refs);
    LinkedList descElement = LinkedList_getNext(descs);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Ind$stVal", (const char*) LinkedList_getData(refElement));
    TEST_ASSERT_EQUAL_STRING("Indication status", (const char*) LinkedList_getData(descElement));

    refElement = LinkedList_getNext(refElement);
    descElement = LinkedList_getNext(descElement);
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Ind$q", (const char*) LinkedList_getData(refElement));
    TEST_ASSERT_NULL(LinkedList_getData(descElement));

    LinkedList_destroyDeep(refs, free);
    LinkedList_destroyStatic(descs); /* borrowed strings - never destroyDeep/free */
    IedModel_destroy(bareModel);
}

/* ---- getDataSetMemberLeafWireTypes / dataAttributeTypeMatchesMmsType ----
 * See mms_report_client_usecases.c's decomposedLeafTypesMatch (and its
 * goose_subscriber twin) for what these back: real production hardware
 * mislabeled a structured DPC's stVal/t leaves because the wire order didn't
 * match this daemon's locally-resolved SCL order, and the pre-existing
 * count-only fallback couldn't catch it. */

void
test_getDataSetMemberLeafWireTypes_decomposesFlatDo_matchesReferenceOrder(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    DataObject* pos = DataObject_create("Pos", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) pos, IEC61850_CODEDENUM, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("t", (ModelNode*) pos, IEC61850_TIMESTAMP, IEC61850_FC_ST, 0, 0, 0);

    DataSet* dataSet = DataSet_create("ds1", ln0);
    DataSetEntry_create(dataSet, "BareLD1/LLN0$ST$Pos", -1, NULL);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafWireTypes(&bareHandle, "BareLD1/LLN0$ds1", 0);

    /* Order matches getDataSetMemberLeafReferences' own traversal exactly
     * (stVal then t - same walk). */
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(leaves));
    LinkedList element = LinkedList_getNext(leaves);
    TEST_ASSERT_EQUAL(IEC61850_CODEDENUM, *(DataAttributeType*) LinkedList_getData(element));
    element = LinkedList_getNext(element);
    TEST_ASSERT_EQUAL(IEC61850_TIMESTAMP, *(DataAttributeType*) LinkedList_getData(element));

    LinkedList_destroyDeep(leaves, free);
    IedModel_destroy(bareModel);
}

void
test_getDataSetMemberLeafWireTypes_empty_whenMemberIsAlreadyLeafLevel(void) {
    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafWireTypes(handle, "TestIEDLD1/LLN0$events", 0);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(leaves));
    LinkedList_destroyDeep(leaves, free);
}

void
test_getDataSetMemberLeafWireTypes_empty_whenDatasetReferenceIsNull(void) {
    LinkedList leaves = IedModelUseCases_getDataSetMemberLeafWireTypes(handle, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(leaves));
    LinkedList_destroyDeep(leaves, free);
}

void
test_dataAttributeTypeMatchesMmsType_confidentMatches(void) {
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_BOOLEAN, MMS_BOOLEAN));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_TIMESTAMP, MMS_UTC_TIME));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_QUALITY, MMS_BIT_STRING));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_CODEDENUM, MMS_BIT_STRING));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_INT32, MMS_INTEGER));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_FLOAT32, MMS_FLOAT));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_VISIBLE_STRING_255, MMS_VISIBLE_STRING));
}

void
test_dataAttributeTypeMatchesMmsType_confidentMismatches(void) {
    /* The exact real-hardware finding this guards against: a UTC_TIME value
     * landing where a coded/bit-string status was expected, and a BOOLEAN
     * landing where a timestamp was expected. */
    TEST_ASSERT_FALSE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_CODEDENUM, MMS_UTC_TIME));
    TEST_ASSERT_FALSE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_TIMESTAMP, MMS_BOOLEAN));
    TEST_ASSERT_FALSE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_BOOLEAN, MMS_UTC_TIME));
    TEST_ASSERT_FALSE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_QUALITY, MMS_BOOLEAN));
}

void
test_dataAttributeTypeMatchesMmsType_unmodeledTypeAlwaysMatches(void) {
    /* Not confident enough to assert - must never reject, regardless of
     * actual wire type, per this codebase's "don't guess IEC 61850
     * semantics" rule. */
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_UNKNOWN_TYPE, MMS_BOOLEAN));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_UNKNOWN_TYPE, MMS_UTC_TIME));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_OCTET_STRING_64, MMS_OCTET_STRING));
    TEST_ASSERT_TRUE(IedModelUseCases_dataAttributeTypeMatchesMmsType(IEC61850_CONSTRUCTED, MMS_STRUCTURE));
}

/* ---- Reportable attribute references for a dynamic-dataset LN ---- */

void
test_getReportableAttributeReferencesForLogicalNode_returnsStAndMxLeavesUnderThatLnOnly(void) {
    /* Fixture LLN0: Mod{stVal(ST), ctlModel(CF)}, TotW{mag(MX)}, CSWI{Oper(CO)} -
     * only stVal/mag qualify, in "LD/LN$FC$DO$DA" form (createDataSet's own
     * wire-format conversion happens one layer up, in mms_report_client). */
    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForLogicalNode(handle, "TestIEDLD1/LLN0");

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(refs));

    bool foundStVal = false, foundMag = false;
    LinkedList element = LinkedList_getNext(refs);
    while (element) {
        const char* ref = (const char*) LinkedList_getData(element);
        if (strcmp(ref, "TestIEDLD1/LLN0$ST$Mod$stVal") == 0) foundStVal = true;
        if (strcmp(ref, "TestIEDLD1/LLN0$MX$TotW$mag") == 0) foundMag = true;
        TEST_ASSERT_NULL_MESSAGE(strstr(ref, "ctlModel"), "CF-scoped ctlModel must never appear");
        TEST_ASSERT_NULL_MESSAGE(strstr(ref, "Oper"), "CO-scoped Oper must never appear");
        element = LinkedList_getNext(element);
    }
    TEST_ASSERT_TRUE(foundStVal);
    TEST_ASSERT_TRUE(foundMag);

    LinkedList_destroyDeep(refs, free);
}

void
test_getReportableAttributeReferencesForLogicalNode_recursesIntoConstructedAttribute(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    DataObject* phV = DataObject_create("PhV", (ModelNode*) ln0, 0);
    DataAttribute* cVal = DataAttribute_create("cVal", (ModelNode*) phV, IEC61850_CONSTRUCTED, IEC61850_FC_MX, 0, 0, 0);
    DataAttribute_create("mag", (ModelNode*) cVal, IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForLogicalNode(&bareHandle, "BareLD1/LLN0");

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(refs));
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$MX$PhV$cVal$mag", firstElement(refs));

    LinkedList_destroyDeep(refs, free);
    IedModel_destroy(bareModel);
}

void
test_getReportableAttributeReferencesForLogicalNode_empty_whenLnReferenceIsNull(void) {
    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForLogicalNode(handle, NULL);

    TEST_ASSERT_NOT_NULL(refs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(refs));

    LinkedList_destroyDeep(refs, free);
}

void
test_getReportableAttributeReferencesForLogicalNode_empty_whenLdDoesNotResolve(void) {
    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForLogicalNode(handle, "NoSuchLD/LLN0");

    TEST_ASSERT_NOT_NULL(refs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(refs));

    LinkedList_destroyDeep(refs, free);
}

void
test_getReportableAttributeReferencesForLogicalNode_empty_whenLnDoesNotResolve(void) {
    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForLogicalNode(handle, "TestIEDLD1/NoSuchLN");

    TEST_ASSERT_NOT_NULL(refs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(refs));

    LinkedList_destroyDeep(refs, free);
}

void
test_getReportableAttributeReferencesForLogicalNode_empty_whenNoSlashInReference(void) {
    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForLogicalNode(handle, "MalformedNoSlash");

    TEST_ASSERT_NOT_NULL(refs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(refs));

    LinkedList_destroyDeep(refs, free);
}

/* ---- Reportable attribute references for whole-device dynamic-dataset clustering ---- */

void
test_getReportableAttributeReferencesForWholeDevice_combinesEveryLdAndLn(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld1 = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ld1Ln0 = LogicalNode_create("LLN0", ld1);
    DataObject* ld1Mod = DataObject_create("Mod", (ModelNode*) ld1Ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) ld1Mod, IEC61850_INT32, IEC61850_FC_ST, 0, 0, 0);

    LogicalDevice* ld2 = LogicalDevice_create("LD2", bareModel);
    LogicalNode* ld2User1 = LogicalNode_create("USER1", ld2);
    DataObject* ld2Ind = DataObject_create("Ind1", (ModelNode*) ld2User1, 0);
    DataAttribute_create("mag", (ModelNode*) ld2Ind, IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    /* Proves whole-device coverage isn't limited to LNs that happen to host an
     * RCB - LD2/USER1 has no RCB anywhere in this fixture, yet its own leaf
     * still surfaces here, exactly like LD1/LLN0's does. */
    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForWholeDevice(&bareHandle);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(refs));

    bool foundLd1StVal = false, foundLd2Mag = false;
    LinkedList element = LinkedList_getNext(refs);
    while (element) {
        const char* ref = (const char*) LinkedList_getData(element);
        if (strcmp(ref, "BareLD1/LLN0$ST$Mod$stVal") == 0) foundLd1StVal = true;
        if (strcmp(ref, "BareLD2/USER1$MX$Ind1$mag") == 0) foundLd2Mag = true;
        element = LinkedList_getNext(element);
    }
    TEST_ASSERT_TRUE_MESSAGE(foundLd1StVal, "expected LD1/LLN0's own leaf to be included");
    TEST_ASSERT_TRUE_MESSAGE(foundLd2Mag, "expected LD2/USER1's leaf to be included too, despite no RCB there");

    LinkedList_destroyDeep(refs, free);
    IedModel_destroy(bareModel);
}

void
test_getReportableAttributeReferencesForWholeDevice_matchesPerLnResult_onSingleLdLnFixture(void) {
    /* Shared fixture (`handle`) has exactly one LD/LN, so the whole-device
     * result must be identical to the per-LN result on that same LN. */
    LinkedList wholeDevice = IedModelUseCases_getReportableAttributeReferencesForWholeDevice(handle);
    LinkedList perLn = IedModelUseCases_getReportableAttributeReferencesForLogicalNode(handle, "TestIEDLD1/LLN0");

    TEST_ASSERT_EQUAL_INT(LinkedList_size(perLn), LinkedList_size(wholeDevice));
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(wholeDevice));

    LinkedList_destroyDeep(wholeDevice, free);
    LinkedList_destroyDeep(perLn, free);
}

void
test_getReportableAttributeReferencesForWholeDevice_empty_whenHandleIsNull(void) {
    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForWholeDevice(NULL);

    TEST_ASSERT_NOT_NULL(refs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(refs));

    LinkedList_destroyDeep(refs, free);
}

/* ---- Read targets ---- */

void
test_getReadTargets_includesOnlyStAndMxAttributes(void) {
    LinkedList targets = IedModelUseCases_getReadTargets(handle);

    /* stVal (ST) and mag (MX) - exactly 2, ctlModel (CF) and Oper (CO) excluded. */
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(targets));

    LinkedList element = LinkedList_getNext(targets);
    bool foundStVal = false, foundMag = false, foundCtlModel = false, foundOper = false;
    while (element) {
        const char* ref = (const char*) LinkedList_getData(element);
        if (strstr(ref, "stVal")) foundStVal = true;
        if (strstr(ref, "mag")) foundMag = true;
        if (strstr(ref, "ctlModel")) foundCtlModel = true;
        if (strstr(ref, "Oper")) foundOper = true;
        element = LinkedList_getNext(element);
    }

    TEST_ASSERT_TRUE(foundStVal);
    TEST_ASSERT_TRUE(foundMag);
    TEST_ASSERT_FALSE(foundCtlModel);
    TEST_ASSERT_FALSE(foundOper);

    LinkedList_destroyDeep(targets, free);
}

void
test_getReadTargets_empty_whenModelHasNoStOrMxAttributes(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    DataObject* cf = DataObject_create("Cfg", (ModelNode*) ln0, 0);
    DataAttribute_create("setting", (ModelNode*) cf, IEC61850_INT32, IEC61850_FC_CF, 0, 0, 0);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_READ_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getReadTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(targets));

    LinkedList_destroyDeep(targets, free);
    IedModel_destroy(bareModel);
}

/* ---- Control targets ---- */

void
test_getControlTargets_includesOnlyDataObjectsWithCoChild(void) {
    LinkedList targets = IedModelUseCases_getControlTargets(handle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(targets));
    TEST_ASSERT_TRUE(strstr(firstElement(targets), "CSWI") != NULL);

    LinkedList_destroyDeep(targets, free);
}

void
test_getControlTargets_empty_whenModelHasNoControllableDataObjects(void) {
    IedModel* bareModel = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", bareModel);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    DataObject* mod = DataObject_create("Mod", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) mod, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_READ_AND_WRITE, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getControlTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(targets));

    LinkedList_destroyDeep(targets, free);
    IedModel_destroy(bareModel);
}

/* ---- categoryFilter ---- */

/* Two LNs, deliberately different classes (XCBR->CONTROL, MMXU->MEASUREMENT,
 * per IedModelLnCategory_forLnClass's own table), each with a GoCB, an RCB,
 * and one FC=ST/MX leaf - enough to exercise all three filtered getters
 * against the same fixture. Caller sets bareHandle's lnCategories/
 * lnCategoryCount/categoryFilter itself (this only builds the model + LNs). */
static IedModel*
buildTwoCategoryFixtureModel(LogicalNode** outXcbrLn, LogicalNode** outMmxuLn) {
    IedModel* model = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", model);

    LogicalNode* xcbrLn = LogicalNode_create("XCBR1", ld);
    DataObject* pos = DataObject_create("Pos", (ModelNode*) xcbrLn, 0);
    DataAttribute_create("stVal", (ModelNode*) pos, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);
    DataSet* xcbrDs = DataSet_create("dsX", xcbrLn);
    DataSetEntry_create(xcbrDs, "BareLD1/XCBR1$ST$Pos$stVal", -1, NULL);
    ReportControlBlock_create("brcbX", xcbrLn, "rptX", true, "dsX", 1, TRG_OPT_DATA_CHANGED, RPT_OPT_SEQ_NUM, 0, 0);
    GSEControlBlock_create("gcbX", xcbrLn, "1000", "dsX", 1, false, -1, -1);

    LogicalNode* mmxuLn = LogicalNode_create("MMXU1", ld);
    DataObject* totW = DataObject_create("TotW", (ModelNode*) mmxuLn, 0);
    DataAttribute_create("mag", (ModelNode*) totW, IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);
    DataSet* mmxuDs = DataSet_create("dsM", mmxuLn);
    DataSetEntry_create(mmxuDs, "BareLD1/MMXU1$MX$TotW$mag", -1, NULL);
    ReportControlBlock_create("brcbM", mmxuLn, "rptM", true, "dsM", 1, TRG_OPT_DATA_CHANGED, RPT_OPT_SEQ_NUM, 0, 0);
    GSEControlBlock_create("gcbM", mmxuLn, "1001", "dsM", 1, false, -1, -1);

    if (outXcbrLn) *outXcbrLn = xcbrLn;
    if (outMmxuLn) *outMmxuLn = mmxuLn;
    return model;
}

/* RCB/GoCB visibility is deliberately NOT gated by categoryFilter - the
 * daemon always needs every RCB/GoCB visible to know where every dataset
 * lives (real SCL very commonly parents every RCB/GoCB on LLN0, which would
 * make a parent-LN gate useless on such hardware - confirmed against a real
 * DIGSI 5/SIPROTEC 6MD85 station file). Category filtering happens
 * per-point instead, downstream in mms_report_client/goose_subscriber - see
 * their own collectCandidates tests. */
void
test_getGooseSubscriptionTargets_ignoresCategoryMask_alwaysIncludesEveryGcb(void) {
    LogicalNode* xcbrLn;
    LogicalNode* mmxuLn;
    IedModel* bareModel = buildTwoCategoryFixtureModel(&xcbrLn, &mmxuLn);
    IedModelLnCategoryEntry categories[] = {
        { .ln = xcbrLn, .category = IED_MODEL_LN_CATEGORY_CONTROL },
        { .ln = mmxuLn, .category = IED_MODEL_LN_CATEGORY_MEASUREMENT },
    };
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_MEASUREMENT, .lnCategories = categories, .lnCategoryCount = 2,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getGooseSubscriptionTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(targets));

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyGooseSubscriptionTarget);
    IedModel_destroy(bareModel);
}

void
test_getReportSubscriptionTargets_ignoresCategoryMask_alwaysIncludesEveryRcb(void) {
    LogicalNode* xcbrLn;
    LogicalNode* mmxuLn;
    IedModel* bareModel = buildTwoCategoryFixtureModel(&xcbrLn, &mmxuLn);
    IedModelLnCategoryEntry categories[] = {
        { .ln = xcbrLn, .category = IED_MODEL_LN_CATEGORY_CONTROL },
        { .ln = mmxuLn, .category = IED_MODEL_LN_CATEGORY_MEASUREMENT },
    };
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_CONTROL, .lnCategories = categories, .lnCategoryCount = 2,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getReportSubscriptionTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(targets));

    LinkedList_destroyDeep(targets, IedModelUseCases_destroyReportControlBlockTarget);
    IedModel_destroy(bareModel);
}

void
test_getReportableAttributeReferencesForWholeDevice_filtersByCategoryMask_excludesNonMatchingLn(void) {
    LogicalNode* xcbrLn;
    LogicalNode* mmxuLn;
    IedModel* bareModel = buildTwoCategoryFixtureModel(&xcbrLn, &mmxuLn);
    IedModelLnCategoryEntry categories[] = {
        { .ln = xcbrLn, .category = IED_MODEL_LN_CATEGORY_CONTROL },
        { .ln = mmxuLn, .category = IED_MODEL_LN_CATEGORY_MEASUREMENT },
    };
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_CONTROL, .lnCategories = categories, .lnCategoryCount = 2,
        .iedName = "Bare" };

    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForWholeDevice(&bareHandle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(refs));
    TEST_ASSERT_EQUAL_STRING("BareLD1/XCBR1$ST$Pos$stVal", firstElement(refs));

    LinkedList_destroyDeep(refs, free);
    IedModel_destroy(bareModel);
}

/* The whole-device leaf walk (unlike the RCB/GoCB target getters above) DOES
 * still filter, per-LN, since that's a decision about which variables a
 * self-created dynamic dataset should capture - not about RCB/GoCB
 * visibility. An LN with no lnCategories entry at all degrades to OTHER (see
 * categoryForLn's own "not found" doc comment), same as any other
 * unclassified LN. */
void
test_getReportableAttributeReferencesForWholeDevice_unclassifiedLn_defaultsToOther_excludedUnlessOtherSelected(void) {
    LogicalNode* xcbrLn;
    LogicalNode* mmxuLn;
    IedModel* bareModel = buildTwoCategoryFixtureModel(&xcbrLn, &mmxuLn);
    /* No lnCategories entries at all - every LN degrades to OTHER. */
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_CONTROL, .lnCategories = NULL, .lnCategoryCount = 0,
        .iedName = "Bare" };

    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForWholeDevice(&bareHandle);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(refs));
    LinkedList_destroyDeep(refs, free);

    bareHandle.categoryFilter = IED_MODEL_LN_CATEGORY_OTHER;
    refs = IedModelUseCases_getReportableAttributeReferencesForWholeDevice(&bareHandle);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(refs));
    LinkedList_destroyDeep(refs, free);

    IedModel_destroy(bareModel);
}

/* ---- LLN0's always-include exemption ---- */

/* Same two-LN shape as buildTwoCategoryFixtureModel, plus an LLN0 carrying
 * the LD-status leaf every filter must let through. */
static IedModel*
buildLln0FixtureModel(LogicalNode** outLln0, LogicalNode** outMmxuLn) {
    IedModel* model = IedModel_create("Bare");
    LogicalDevice* ld = LogicalDevice_create("LD1", model);

    LogicalNode* lln0 = LogicalNode_create("LLN0", ld);
    DataObject* beh = DataObject_create("Beh", (ModelNode*) lln0, 0);
    DataAttribute_create("stVal", (ModelNode*) beh, IEC61850_ENUMERATED, IEC61850_FC_ST, 0, 0, 0);

    LogicalNode* mmxuLn = LogicalNode_create("MMXU1", ld);
    DataObject* totW = DataObject_create("TotW", (ModelNode*) mmxuLn, 0);
    DataAttribute_create("mag", (ModelNode*) totW, IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);

    if (outLln0) *outLln0 = lln0;
    if (outMmxuLn) *outMmxuLn = mmxuLn;
    return model;
}

void
test_getReportableAttributeReferencesForWholeDevice_lln0SurvivesAFilterExcludingItsCategory(void) {
    LogicalNode* lln0;
    LogicalNode* mmxuLn;
    IedModel* bareModel = buildLln0FixtureModel(&lln0, &mmxuLn);
    /* LLN0 still classifies OTHER (that's what goes on the wire) - only
     * alwaysInclude exempts it from the mask. */
    IedModelLnCategoryEntry categories[] = {
        { .ln = lln0, .category = IED_MODEL_LN_CATEGORY_OTHER, .alwaysInclude = true },
        { .ln = mmxuLn, .category = IED_MODEL_LN_CATEGORY_MEASUREMENT, .alwaysInclude = false },
    };
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_CONTROL, .lnCategories = categories, .lnCategoryCount = 2,
        .iedName = "Bare" };

    LinkedList refs = IedModelUseCases_getReportableAttributeReferencesForWholeDevice(&bareHandle);

    /* CONTROL matches neither LN's category, yet LLN0's leaf is still here. */
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(refs));
    TEST_ASSERT_EQUAL_STRING("BareLD1/LLN0$ST$Beh$stVal", firstElement(refs));

    LinkedList_destroyDeep(refs, free);
    IedModel_destroy(bareModel);
}

void
test_isMemberReferenceAlwaysIncluded_trueOnlyForAnExemptLn(void) {
    LogicalNode* lln0;
    LogicalNode* mmxuLn;
    IedModel* bareModel = buildLln0FixtureModel(&lln0, &mmxuLn);
    IedModelLnCategoryEntry categories[] = {
        { .ln = lln0, .category = IED_MODEL_LN_CATEGORY_OTHER, .alwaysInclude = true },
        { .ln = mmxuLn, .category = IED_MODEL_LN_CATEGORY_MEASUREMENT, .alwaysInclude = false },
    };
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
        .categoryFilter = IED_MODEL_LN_CATEGORY_CONTROL, .lnCategories = categories, .lnCategoryCount = 2,
        .iedName = "Bare" };

    TEST_ASSERT_TRUE(IedModelUseCases_isMemberReferenceAlwaysIncluded(&bareHandle, "BareLD1/LLN0$ST$Beh$stVal"));
    /* Resolves at the LD/LN prefix only, so a DO-level reference agrees. */
    TEST_ASSERT_TRUE(IedModelUseCases_isMemberReferenceAlwaysIncluded(&bareHandle, "BareLD1/LLN0$ST$Beh"));
    TEST_ASSERT_FALSE(IedModelUseCases_isMemberReferenceAlwaysIncluded(&bareHandle, "BareLD1/MMXU1$MX$TotW$mag"));

    /* LLN0 keeps its OTHER category - the wire-facing value is untouched. */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER,
            IedModelUseCases_getCategoryForMemberReference(&bareHandle, "BareLD1/LLN0$ST$Beh$stVal"));

    IedModel_destroy(bareModel);
}

/* An LN with no lnCategories entry must never earn an exemption out of thin
 * air - the opposite default from categoryForLn's OTHER, deliberately. */
void
test_isMemberReferenceAlwaysIncluded_falseWhenUnresolvableOrNull(void) {
    TEST_ASSERT_FALSE(IedModelUseCases_isMemberReferenceAlwaysIncluded(handle, "TestIEDLD1/NoSuchLn$ST$Mod$stVal"));
    TEST_ASSERT_FALSE(IedModelUseCases_isMemberReferenceAlwaysIncluded(handle, "not-a-reference"));
    TEST_ASSERT_FALSE(IedModelUseCases_isMemberReferenceAlwaysIncluded(handle, NULL));
    TEST_ASSERT_FALSE(IedModelUseCases_isMemberReferenceAlwaysIncluded(NULL, "TestIEDLD1/LLN0$ST$Beh$stVal"));
}

void
test_getCategoryFilter_returnsHandlesMask(void) {
    struct sIedModelHandle bareHandle = { .categoryFilter = IED_MODEL_LN_CATEGORY_CONTROL | IED_MODEL_LN_CATEGORY_OTHER };
    TEST_ASSERT_EQUAL_INT(IED_MODEL_LN_CATEGORY_CONTROL | IED_MODEL_LN_CATEGORY_OTHER,
            IedModelUseCases_getCategoryFilter(&bareHandle));
}

void
test_getCategoryFilter_nullHandle_returnsAll(void) {
    TEST_ASSERT_EQUAL_INT(IED_MODEL_LN_CATEGORY_ALL, IedModelUseCases_getCategoryFilter(NULL));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_getGooseSubscriptionTargets_returnsCorrectReference);
    RUN_TEST(test_getGooseSubscriptionTargets_empty_whenModelHasNoGseControlBlocks);
    RUN_TEST(test_getGooseSubscriptionTargets_populatesAddress_whenPhyComAddressPresent);
    RUN_TEST(test_getGooseSubscriptionTargets_datasetReferenceNull_whenGcbHasNoDataset);

    RUN_TEST(test_getReportSubscriptionTargets_returnsCorrectReference);
    RUN_TEST(test_getReportSubscriptionTargets_unbufferedRcb_usesRpSegment);
    RUN_TEST(test_getReportSubscriptionTargets_empty_whenModelHasNoRcbs);
    RUN_TEST(test_getReportSubscriptionTargets_datasetReferenceNull_whenRcbHasNoDataset);

    RUN_TEST(test_getDataSetMemberReferences_returnsSingleEntry_forEventsDataset);
    RUN_TEST(test_getDataSetMemberReferences_preservesOrder_forMultiEntryDataset);
    RUN_TEST(test_getDataSetMemberReferences_empty_whenDatasetReferenceIsNull);
    RUN_TEST(test_getDataSetMemberReferences_empty_whenDatasetReferenceDoesNotResolve);

    RUN_TEST(test_getDataSetMemberLeafReferences_empty_whenMemberIsAlreadyLeafLevel);
    RUN_TEST(test_getDataSetMemberLeafReferences_decomposesFlatDo);
    RUN_TEST(test_getDataSetMemberLeafReferences_recursesIntoConstructedAttribute);
    RUN_TEST(test_getDataSetMemberLeafReferences_filtersByFc_excludesOtherFcSiblings);
    RUN_TEST(test_getDataSetMemberLeafReferences_empty_whenDatasetReferenceIsNull);
    RUN_TEST(test_getDataSetMemberLeafReferences_empty_whenIndexOutOfRange);
    RUN_TEST(test_getDataSetMemberLeafReferences_empty_whenIndexNegative);

    RUN_TEST(test_getLeafReferencesForMemberReference_decomposesFlatDo_withNoDataSetRegisteredAtAll);
    RUN_TEST(test_getLeafReferencesForMemberReference_matchesDataSetIndexedAccessor_forSameDo);
    RUN_TEST(test_getLeafReferencesForMemberReference_empty_whenAlreadyLeafLevel);
    RUN_TEST(test_getLeafReferencesForMemberReference_empty_whenNull);
    RUN_TEST(test_getLeafWireTypesForMemberReference_matchesLeafReferences_countAndOrder);
    RUN_TEST(test_getSemanticForMemberReference_none_forOrdinaryLeaf);
    RUN_TEST(test_getSemanticForMemberReference_none_whenDoLevel_notLeaf);
    RUN_TEST(test_getSemanticForMemberReference_none_whenNull);

    RUN_TEST(test_getCategoryForMemberReference_resolvesLnCategory_regardlessOfLeaf);
    RUN_TEST(test_getCategoryForMemberReference_other_whenLnDoesNotResolveOrNull);
    RUN_TEST(test_getDescriptionForMemberReference_returnsCapturedDesc_forMatchingDa);
    RUN_TEST(test_getDescriptionForMemberReference_null_whenNoDescCapturedOrUnresolvedOrNull);
    RUN_TEST(test_getLeafDescriptionsForMemberReference_indexAlignedWithLeafReferences_nullForUncaptured);

    RUN_TEST(test_getDataSetMemberLeafWireTypes_decomposesFlatDo_matchesReferenceOrder);
    RUN_TEST(test_getDataSetMemberLeafWireTypes_empty_whenMemberIsAlreadyLeafLevel);
    RUN_TEST(test_getDataSetMemberLeafWireTypes_empty_whenDatasetReferenceIsNull);
    RUN_TEST(test_dataAttributeTypeMatchesMmsType_confidentMatches);
    RUN_TEST(test_dataAttributeTypeMatchesMmsType_confidentMismatches);
    RUN_TEST(test_dataAttributeTypeMatchesMmsType_unmodeledTypeAlwaysMatches);

    RUN_TEST(test_getReportableAttributeReferencesForLogicalNode_returnsStAndMxLeavesUnderThatLnOnly);
    RUN_TEST(test_getReportableAttributeReferencesForLogicalNode_recursesIntoConstructedAttribute);
    RUN_TEST(test_getReportableAttributeReferencesForLogicalNode_empty_whenLnReferenceIsNull);
    RUN_TEST(test_getReportableAttributeReferencesForLogicalNode_empty_whenLdDoesNotResolve);
    RUN_TEST(test_getReportableAttributeReferencesForLogicalNode_empty_whenLnDoesNotResolve);
    RUN_TEST(test_getReportableAttributeReferencesForLogicalNode_empty_whenNoSlashInReference);

    RUN_TEST(test_getReportableAttributeReferencesForWholeDevice_combinesEveryLdAndLn);
    RUN_TEST(test_getReportableAttributeReferencesForWholeDevice_matchesPerLnResult_onSingleLdLnFixture);
    RUN_TEST(test_getReportableAttributeReferencesForWholeDevice_empty_whenHandleIsNull);

    RUN_TEST(test_getReadTargets_includesOnlyStAndMxAttributes);
    RUN_TEST(test_getReadTargets_empty_whenModelHasNoStOrMxAttributes);

    RUN_TEST(test_getControlTargets_includesOnlyDataObjectsWithCoChild);
    RUN_TEST(test_getControlTargets_empty_whenModelHasNoControllableDataObjects);

    RUN_TEST(test_getGooseSubscriptionTargets_ignoresCategoryMask_alwaysIncludesEveryGcb);
    RUN_TEST(test_getReportSubscriptionTargets_ignoresCategoryMask_alwaysIncludesEveryRcb);
    RUN_TEST(test_getReportableAttributeReferencesForWholeDevice_filtersByCategoryMask_excludesNonMatchingLn);
    RUN_TEST(test_getReportableAttributeReferencesForWholeDevice_unclassifiedLn_defaultsToOther_excludedUnlessOtherSelected);
    RUN_TEST(test_getReportableAttributeReferencesForWholeDevice_lln0SurvivesAFilterExcludingItsCategory);
    RUN_TEST(test_isMemberReferenceAlwaysIncluded_trueOnlyForAnExemptLn);
    RUN_TEST(test_isMemberReferenceAlwaysIncluded_falseWhenUnresolvableOrNull);

    RUN_TEST(test_getCategoryFilter_returnsHandlesMask);
    RUN_TEST(test_getCategoryFilter_nullHandle_returnsAll);

    return UNITY_END();
}
