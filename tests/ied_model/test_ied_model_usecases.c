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
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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
    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_REPORT_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_READ_ONLY,
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

    struct sIedModelHandle bareHandle = { .model = bareModel, .accessMode = IED_MODEL_ACCESS_READ_AND_WRITE,
        .iedName = "Bare" };

    LinkedList targets = IedModelUseCases_getControlTargets(&bareHandle);

    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(targets));

    LinkedList_destroyDeep(targets, free);
    IedModel_destroy(bareModel);
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

    RUN_TEST(test_getReadTargets_includesOnlyStAndMxAttributes);
    RUN_TEST(test_getReadTargets_empty_whenModelHasNoStOrMxAttributes);

    RUN_TEST(test_getControlTargets_includesOnlyDataObjectsWithCoChild);
    RUN_TEST(test_getControlTargets_empty_whenModelHasNoControllableDataObjects);

    return UNITY_END();
}
