#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ied_model/service/ied_model_api.h"
#include "iec61850_dynamic_model.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- minimal self-contained SCL fixture, written to a temp file per test ----
 *
 * Deliberately tiny (one LD, one LN0, one ST attribute) - this exists only to
 * prove IedModel_loadFromFile's own wiring (open file, delegate to the loader,
 * populate the handle) works, not to exercise the loader's real-world parsing
 * breadth. That's the E2E test's job (integration_tests/, against a real
 * substantial file). Self-contained per test (write + unlink) so this stays a
 * hermetic unit test with no committed fixture file. */
static const char* MINIMAL_SCL =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<SCL xmlns=\"http://www.iec.ch/61850/2003/SCL\">\n"
        "  <IED name=\"TestIED\">\n"
        "    <AccessPoint name=\"AP1\">\n"
        "      <Server>\n"
        "        <LDevice inst=\"LD1\">\n"
        "          <LN0 lnClass=\"LLN0\" lnType=\"T1\" inst=\"\" />\n"
        "        </LDevice>\n"
        "      </Server>\n"
        "    </AccessPoint>\n"
        "  </IED>\n"
        "  <DataTypeTemplates>\n"
        "    <LNodeType id=\"T1\" lnClass=\"LLN0\">\n"
        "      <DO name=\"Mod\" type=\"DOT1\" />\n"
        "    </LNodeType>\n"
        "    <DOType id=\"DOT1\" cdc=\"INC\">\n"
        "      <DA name=\"stVal\" bType=\"BOOLEAN\" fc=\"ST\" />\n"
        "    </DOType>\n"
        "  </DataTypeTemplates>\n"
        "</SCL>\n";

static char*
writeTempFile(const char* contents) {
    char* path = strdup("/tmp/ied_model_test_XXXXXX");
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, "failed to create temp SCL fixture file");

    FILE* fp = fdopen(fd, "w");
    fputs(contents, fp);
    fclose(fp);

    return path;
}

static char*
writeTempSclFile(void) {
    return writeTempFile(MINIMAL_SCL);
}

/* Two <IED> elements - proves IedModelSclLoader_listIedNames doesn't stop at
 * the first match (unlike mxmlFindElement, which only returns one). */
static const char* TWO_IED_SCL =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<SCL xmlns=\"http://www.iec.ch/61850/2003/SCL\">\n"
        "  <IED name=\"FirstIED\" />\n"
        "  <IED name=\"SecondIED\" />\n"
        "</SCL>\n";

/* Zero <IED> elements - a valid, non-error parse result. */
static const char* ZERO_IED_SCL =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<SCL xmlns=\"http://www.iec.ch/61850/2003/SCL\">\n"
        "  <Header id=\"none\" />\n"
        "</SCL>\n";

/* ---- IedModel_loadFromFile ---- */

void
test_loadFromFile_errorFileNotFound_whenPathDoesNotExist(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile("/nonexistent/path/should/not/exist.icd", "TestIED",
            IED_MODEL_ACCESS_READ_ONLY, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(IED_MODEL_ERR_FILE_NOT_FOUND, error);
}

void
test_loadFromFile_errorIedNotFound_whenIedNameDoesNotMatch(void) {
    char* path = writeTempSclFile();

    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(path, "NoSuchIed", IED_MODEL_ACCESS_READ_ONLY, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(IED_MODEL_ERR_IED_NOT_FOUND, error);

    unlink(path);
    free(path);
}

void
test_loadFromFile_success_wiresModelAndAccessModeIntoHandle(void) {
    char* path = writeTempSclFile();

    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(path, "TestIED", IED_MODEL_ACCESS_READ_ONLY, &error);

    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

    /* Real wiring proof: the read target built from the actually-parsed model
     * is reachable through the handle, not a stub. */
    LinkedList reads = IedModel_getReadTargets(handle);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(reads));
    LinkedList_destroyDeep(reads, free);

    IedModel_release(handle);
    unlink(path);
    free(path);
}

void
test_release_doesNotCrash_onNullHandle(void) {
    IedModel_release(NULL); /* must not crash - this is the assertion */
    TEST_PASS();
}

/* ---- IedModel_listIedNames ---- */

void
test_listIedNames_errorFileNotFound_whenPathDoesNotExist(void) {
    IedModelLoadError error;
    LinkedList names = IedModel_listIedNames("/nonexistent/path/should/not/exist.icd", &error);

    TEST_ASSERT_NULL(names);
    TEST_ASSERT_EQUAL(IED_MODEL_ERR_FILE_NOT_FOUND, error);
}

void
test_listIedNames_returnsOne_forSingleIedFixture(void) {
    char* path = writeTempSclFile();

    IedModelLoadError error;
    LinkedList names = IedModel_listIedNames(path, &error);

    TEST_ASSERT_NOT_NULL(names);
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(names));
    TEST_ASSERT_EQUAL_STRING("TestIED", (char*) LinkedList_getData(LinkedList_getNext(names)));

    LinkedList_destroyDeep(names, free);
    unlink(path);
    free(path);
}

void
test_listIedNames_returnsTwo_forTwoIedFixture(void) {
    char* path = writeTempFile(TWO_IED_SCL);

    IedModelLoadError error;
    LinkedList names = IedModel_listIedNames(path, &error);

    TEST_ASSERT_NOT_NULL(names);
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(names));

    LinkedList element = LinkedList_getNext(names);
    TEST_ASSERT_EQUAL_STRING("FirstIED", (char*) LinkedList_getData(element));
    element = LinkedList_getNext(element);
    TEST_ASSERT_EQUAL_STRING("SecondIED", (char*) LinkedList_getData(element));

    LinkedList_destroyDeep(names, free);
    unlink(path);
    free(path);
}

void
test_listIedNames_returnsEmpty_forZeroIedFixture(void) {
    char* path = writeTempFile(ZERO_IED_SCL);

    IedModelLoadError error;
    LinkedList names = IedModel_listIedNames(path, &error);

    TEST_ASSERT_NOT_NULL(names);
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(names));

    LinkedList_destroyDeep(names, free);
    unlink(path);
    free(path);
}

/* ---- AccessMode gating ----
 *
 * Directly constructs a handle over a tiny in-memory model (no file I/O) so
 * these stay fast and isolated to api.c's own gating logic, independent of
 * loader correctness. */

static IedModel*
buildGatingFixtureModel(void) {
    IedModel* model = IedModel_create("TestIED");
    LogicalDevice* ld = LogicalDevice_create("LD1", model);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);

    DataObject* mod = DataObject_create("Mod", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) mod, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);

    DataObject* cswi = DataObject_create("CSWI", (ModelNode*) ln0, 0);
    DataAttribute_create("Oper", (ModelNode*) cswi, IEC61850_BOOLEAN, IEC61850_FC_CO, 0, 0, 0);

    DataSet* dataSet = DataSet_create("events", ln0);
    DataSetEntry_create(dataSet, "TestIEDLD1/LLN0$ST$Mod$stVal", -1, NULL);

    ReportControlBlock_create("brcb01", ln0, "rpt01", true, "events", 1, TRG_OPT_DATA_CHANGED, RPT_OPT_SEQ_NUM, 0, 0);
    GSEControlBlock_create("gcb01", ln0, "1000", "events", 1, false, -1, -1);

    return model;
}

static IedModelHandle
makeHandle(IedModel* model, AccessMode mode) {
    struct sIedModelHandle* handle = malloc(sizeof(struct sIedModelHandle));
    handle->model = model;
    handle->accessMode = mode;
    handle->iedName = "TestIED";
    handle->daSemantics = NULL;
    handle->daSemanticCount = 0;
    return handle;
}

void
test_gating_reportOnly_allowsGooseAndReport_deniesReadAndControl(void) {
    IedModel* model = buildGatingFixtureModel();
    IedModelHandle handle = makeHandle(model, IED_MODEL_ACCESS_REPORT_ONLY);

    LinkedList goose = IedModel_getGooseSubscriptionTargets(handle);
    LinkedList report = IedModel_getReportSubscriptionTargets(handle);
    LinkedList read = IedModel_getReadTargets(handle);
    LinkedList control = IedModel_getControlTargets(handle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(goose));
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(report));
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(read));
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(control));

    LinkedList_destroyDeep(goose, IedModel_destroyGooseSubscriptionTarget);
    LinkedList_destroyDeep(report, IedModel_destroyReportControlBlockTarget);
    LinkedList_destroyDeep(read, free);
    LinkedList_destroyDeep(control, free);
    free(handle);
    IedModel_destroy(model);
}

void
test_gating_readOnly_allowsRead_stillDeniesControl(void) {
    IedModel* model = buildGatingFixtureModel();
    IedModelHandle handle = makeHandle(model, IED_MODEL_ACCESS_READ_ONLY);

    LinkedList read = IedModel_getReadTargets(handle);
    LinkedList control = IedModel_getControlTargets(handle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(read));
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(control));

    LinkedList_destroyDeep(read, free);
    LinkedList_destroyDeep(control, free);
    free(handle);
    IedModel_destroy(model);
}

void
test_gating_readAndWrite_allowsEverything(void) {
    IedModel* model = buildGatingFixtureModel();
    IedModelHandle handle = makeHandle(model, IED_MODEL_ACCESS_READ_AND_WRITE);

    LinkedList goose = IedModel_getGooseSubscriptionTargets(handle);
    LinkedList report = IedModel_getReportSubscriptionTargets(handle);
    LinkedList read = IedModel_getReadTargets(handle);
    LinkedList control = IedModel_getControlTargets(handle);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(goose));
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(report));
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(read));
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(control));

    LinkedList_destroyDeep(goose, IedModel_destroyGooseSubscriptionTarget);
    LinkedList_destroyDeep(report, IedModel_destroyReportControlBlockTarget);
    LinkedList_destroyDeep(read, free);
    LinkedList_destroyDeep(control, free);
    free(handle);
    IedModel_destroy(model);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_loadFromFile_errorFileNotFound_whenPathDoesNotExist);
    RUN_TEST(test_loadFromFile_errorIedNotFound_whenIedNameDoesNotMatch);
    RUN_TEST(test_loadFromFile_success_wiresModelAndAccessModeIntoHandle);
    RUN_TEST(test_release_doesNotCrash_onNullHandle);

    RUN_TEST(test_listIedNames_errorFileNotFound_whenPathDoesNotExist);
    RUN_TEST(test_listIedNames_returnsOne_forSingleIedFixture);
    RUN_TEST(test_listIedNames_returnsTwo_forTwoIedFixture);
    RUN_TEST(test_listIedNames_returnsEmpty_forZeroIedFixture);

    RUN_TEST(test_gating_reportOnly_allowsGooseAndReport_deniesReadAndControl);
    RUN_TEST(test_gating_readOnly_allowsRead_stillDeniesControl);
    RUN_TEST(test_gating_readAndWrite_allowsEverything);

    return UNITY_END();
}
