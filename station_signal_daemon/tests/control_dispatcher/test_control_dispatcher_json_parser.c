#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/control_dispatcher/data/control_dispatcher_json_parser.h"
#include "features/control_dispatcher/data/control_dispatcher_request_queue.h"

static char* fixtureRequestId;
static char* fixtureAction;
static ControlRequest* fixtureRequest;

void
setUp(void) {
    fixtureRequestId = NULL;
    fixtureAction = NULL;
    fixtureRequest = NULL;
}

void
tearDown(void) {
    free(fixtureRequestId);
    free(fixtureAction);
    ControlDispatcherRequest_destroy(fixtureRequest);
}

static ControlParseError
parse(const char* json) {
    return ControlDispatcherJsonParser_parse(json, strlen(json), &fixtureRequestId, &fixtureAction, &fixtureRequest);
}

/* ---- malformed / structural errors ---- */

void
test_malformedJson_returnsMalformedError(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_MALFORMED_JSON, parse("{not valid json"));
    TEST_ASSERT_NULL(fixtureRequest);
}

void
test_nonObjectRoot_returnsMalformedError(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_MALFORMED_JSON, parse("[1,2,3]"));
}

void
test_missingRequestId_returnsMissingRequestIdError_recoversAction(void) {
    ControlParseError err = parse("{\"action\":\"START_REPORTING\",\"params\":{}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_MISSING_REQUEST_ID, err);
    TEST_ASSERT_NULL(fixtureRequestId);
    TEST_ASSERT_NOT_NULL(fixtureAction);
    TEST_ASSERT_EQUAL_STRING("START_REPORTING", fixtureAction);
}

void
test_emptyRequestId_returnsMissingRequestIdError(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_MISSING_REQUEST_ID,
            parse("{\"requestId\":\"\",\"action\":\"START_REPORTING\",\"params\":{}}"));
}

void
test_unknownAction_returnsUnknownActionError_recoversRequestIdAndAction(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"DO_A_BARREL_ROLL\",\"params\":{}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_UNKNOWN_ACTION, err);
    TEST_ASSERT_NOT_NULL(fixtureRequestId);
    TEST_ASSERT_EQUAL_STRING("req-1", fixtureRequestId);
    TEST_ASSERT_NOT_NULL(fixtureAction);
    TEST_ASSERT_EQUAL_STRING("DO_A_BARREL_ROLL", fixtureAction);
}

void
test_missingAction_returnsUnknownActionError(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_UNKNOWN_ACTION, parse("{\"requestId\":\"req-1\",\"params\":{}}"));
}

void
test_missingParams_returnsInvalidParamsError(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\"}"));
}

/* ---- START_REPORTING params ---- */

void
test_startReporting_missingHost_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"interfaceId\":\"eth0\"}}"));
}

void
test_startReporting_missingInterfaceId_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"host\":\"10.0.0.1\"}}"));
}

void
test_startReporting_sclFilePathWithoutIedName_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\","
                    "\"sclFilePath\":\"/tmp/x.icd\"}}"));
}

void
test_startReporting_unknownAccessMode_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\","
                    "\"accessMode\":\"SOMETHING_ELSE\"}}"));
}

void
test_startReporting_minimalValid_appliesDefaults(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
            "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\"}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_NOT_NULL(fixtureRequest);
    TEST_ASSERT_EQUAL(CONTROL_REQ_START_REPORTING, fixtureRequest->type);
    TEST_ASSERT_EQUAL_STRING("req-1", fixtureRequest->requestId);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", fixtureRequest->host);
    TEST_ASSERT_EQUAL_STRING("eth0", fixtureRequest->interfaceId);
    TEST_ASSERT_EQUAL_INT(102, fixtureRequest->mmsPort); /* default */
    TEST_ASSERT_NULL(fixtureRequest->iedName);
    TEST_ASSERT_NULL(fixtureRequest->sclFilePath);
    TEST_ASSERT_NULL(fixtureRequest->acseAuthPassword);
    TEST_ASSERT_EQUAL(IED_MODEL_ACCESS_REPORT_ONLY, fixtureRequest->accessMode); /* default */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_ALL, fixtureRequest->lnCategoryFilter); /* default */

    /* Success clears the error-echo recovery fields. */
    TEST_ASSERT_NULL(fixtureRequestId);
    TEST_ASSERT_NULL(fixtureAction);
}

void
test_startReporting_fullyPopulated(void) {
    ControlParseError err = parse("{\"requestId\":\"req-2\",\"action\":\"START_REPORTING\","
            "\"params\":{\"host\":\"10.0.0.1\",\"mmsPort\":103,\"iedName\":\"Reporter1\","
            "\"interfaceId\":\"eth0\",\"sclFilePath\":\"/tmp/x.icd\",\"acseAuthPassword\":\"secret\","
            "\"accessMode\":\"READ_AND_WRITE\",\"lnCategories\":[\"CONTROL\",\"MEASUREMENT\"]}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_NOT_NULL(fixtureRequest);
    TEST_ASSERT_EQUAL_INT(103, fixtureRequest->mmsPort);
    TEST_ASSERT_EQUAL_STRING("Reporter1", fixtureRequest->iedName);
    TEST_ASSERT_EQUAL_STRING("/tmp/x.icd", fixtureRequest->sclFilePath);
    TEST_ASSERT_EQUAL_STRING("secret", fixtureRequest->acseAuthPassword);
    TEST_ASSERT_EQUAL(IED_MODEL_ACCESS_READ_AND_WRITE, fixtureRequest->accessMode);
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL | IED_MODEL_LN_CATEGORY_MEASUREMENT,
            fixtureRequest->lnCategoryFilter);
}

/* ---- lnCategories (START_REPORTING) ---- */

void
test_startReporting_lnCategories_absent_defaultsToAll(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
            "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\"}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_ALL, fixtureRequest->lnCategoryFilter);
}

void
test_startReporting_lnCategories_singleValue(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
            "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\",\"lnCategories\":[\"PROTECTION\"]}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, fixtureRequest->lnCategoryFilter);
}

void
test_startReporting_lnCategories_allFourValues_orsTogether(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
            "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\","
            "\"lnCategories\":[\"CONTROL\",\"MEASUREMENT\",\"PROTECTION\",\"OTHER\"]}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_ALL, fixtureRequest->lnCategoryFilter);
}

void
test_startReporting_lnCategories_emptyArray_returnsInvalidParams(void) {
    /* Fail closed - a caller wanting "no filter" should omit the field
     * entirely; an explicit empty array is more likely a client bug than
     * genuine "subscribe to nothing" intent (explicit product decision). */
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\",\"lnCategories\":[]}}"));
}

void
test_startReporting_lnCategories_unrecognizedCategoryName_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\","
                    "\"lnCategories\":[\"SOMETHING_ELSE\"]}}"));
}

void
test_startReporting_lnCategories_mixedValidAndInvalid_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\","
                    "\"lnCategories\":[\"CONTROL\",\"NOT_A_CATEGORY\"]}}"));
}

void
test_startReporting_lnCategories_notAnArray_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\",\"lnCategories\":\"CONTROL\"}}"));
}

void
test_startReporting_lnCategories_nonStringElement_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_REPORTING\","
                    "\"params\":{\"host\":\"10.0.0.1\",\"interfaceId\":\"eth0\",\"lnCategories\":[42]}}"));
}

/* ---- STOP_REPORTING params ---- */

void
test_stopReporting_missingDeviceId_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"STOP_REPORTING\",\"params\":{}}"));
}

void
test_stopReporting_negativeDeviceId_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"STOP_REPORTING\",\"params\":{\"deviceId\":-1}}"));
}

void
test_stopReporting_valid(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"STOP_REPORTING\","
            "\"params\":{\"deviceId\":3}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_NOT_NULL(fixtureRequest);
    TEST_ASSERT_EQUAL(CONTROL_REQ_STOP_REPORTING, fixtureRequest->type);
    TEST_ASSERT_EQUAL_UINT64(3, fixtureRequest->deviceId);
    TEST_ASSERT_NULL(fixtureRequest->host); /* deviceId form leaves host unset - the discriminator
                                                control_dispatcher_usecases.c relies on */
}

void
test_stopReporting_byAddress_missingHost_returnsInvalidParams(void) {
    /* No deviceId AND no host - neither form satisfied. */
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"STOP_REPORTING\",\"params\":{\"mmsPort\":102}}"));
}

void
test_stopReporting_byAddress_emptyHost_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"STOP_REPORTING\",\"params\":{\"host\":\"\"}}"));
}

void
test_stopReporting_byAddress_valid_appliesDefaultMmsPort(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"STOP_REPORTING\","
            "\"params\":{\"host\":\"10.0.0.1\"}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_NOT_NULL(fixtureRequest);
    TEST_ASSERT_EQUAL(CONTROL_REQ_STOP_REPORTING, fixtureRequest->type);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", fixtureRequest->host);
    TEST_ASSERT_EQUAL(102, fixtureRequest->mmsPort);
    TEST_ASSERT_EQUAL_UINT64(0, fixtureRequest->deviceId); /* unset - see 0-is-never-valid convention */
}

void
test_stopReporting_byAddress_valid_explicitMmsPort(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"STOP_REPORTING\","
            "\"params\":{\"host\":\"10.0.0.1\",\"mmsPort\":10301}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", fixtureRequest->host);
    TEST_ASSERT_EQUAL(10301, fixtureRequest->mmsPort);
}

void
test_stopReporting_deviceIdTakesPrecedenceOverHost_whenBothGiven(void) {
    /* deviceId present (even if also given a host) always wins - the parser
     * checks for the deviceId key first. */
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"STOP_REPORTING\","
            "\"params\":{\"deviceId\":3,\"host\":\"10.0.0.1\"}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_EQUAL_UINT64(3, fixtureRequest->deviceId);
    TEST_ASSERT_NULL(fixtureRequest->host);
}

/* ---- START_SCAN params ---- */

void
test_startScan_missingInterfaceId_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_SCAN\",\"params\":{}}"));
}

void
test_startScan_emptyInterfaceId_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"START_SCAN\",\"params\":{\"interfaceId\":\"\"}}"));
}

void
test_startScan_minimalValid_appliesDefaults(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"START_SCAN\","
            "\"params\":{\"interfaceId\":\"lo\"}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_NOT_NULL(fixtureRequest);
    TEST_ASSERT_EQUAL(CONTROL_REQ_START_SCAN, fixtureRequest->type);
    TEST_ASSERT_EQUAL_STRING("lo", fixtureRequest->interfaceId);
    TEST_ASSERT_EQUAL_INT(102, fixtureRequest->mmsPort); /* default */
    TEST_ASSERT_EQUAL_UINT32(0, fixtureRequest->sweepIntervalMs); /* default = scan_orchestration's own */
}

void
test_startScan_fullyPopulated(void) {
    ControlParseError err = parse("{\"requestId\":\"req-2\",\"action\":\"START_SCAN\","
            "\"params\":{\"interfaceId\":\"eth0\",\"mmsPort\":103,\"sweepIntervalMs\":5000}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_NOT_NULL(fixtureRequest);
    TEST_ASSERT_EQUAL_STRING("eth0", fixtureRequest->interfaceId);
    TEST_ASSERT_EQUAL_INT(103, fixtureRequest->mmsPort);
    TEST_ASSERT_EQUAL_UINT32(5000, fixtureRequest->sweepIntervalMs);
}

/* ---- STOP_SCAN params ---- */

void
test_stopScan_missingScanId_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"STOP_SCAN\",\"params\":{}}"));
}

void
test_stopScan_negativeScanId_returnsInvalidParams(void) {
    TEST_ASSERT_EQUAL(CONTROL_PARSE_ERR_INVALID_PARAMS,
            parse("{\"requestId\":\"req-1\",\"action\":\"STOP_SCAN\",\"params\":{\"scanId\":-1}}"));
}

void
test_stopScan_valid(void) {
    ControlParseError err = parse("{\"requestId\":\"req-1\",\"action\":\"STOP_SCAN\","
            "\"params\":{\"scanId\":7}}");

    TEST_ASSERT_EQUAL(CONTROL_PARSE_OK, err);
    TEST_ASSERT_NOT_NULL(fixtureRequest);
    TEST_ASSERT_EQUAL(CONTROL_REQ_STOP_SCAN, fixtureRequest->type);
    TEST_ASSERT_EQUAL_UINT64(7, fixtureRequest->scanId);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_malformedJson_returnsMalformedError);
    RUN_TEST(test_nonObjectRoot_returnsMalformedError);
    RUN_TEST(test_missingRequestId_returnsMissingRequestIdError_recoversAction);
    RUN_TEST(test_emptyRequestId_returnsMissingRequestIdError);
    RUN_TEST(test_unknownAction_returnsUnknownActionError_recoversRequestIdAndAction);
    RUN_TEST(test_missingAction_returnsUnknownActionError);
    RUN_TEST(test_missingParams_returnsInvalidParamsError);

    RUN_TEST(test_startReporting_missingHost_returnsInvalidParams);
    RUN_TEST(test_startReporting_missingInterfaceId_returnsInvalidParams);
    RUN_TEST(test_startReporting_sclFilePathWithoutIedName_returnsInvalidParams);
    RUN_TEST(test_startReporting_unknownAccessMode_returnsInvalidParams);
    RUN_TEST(test_startReporting_minimalValid_appliesDefaults);
    RUN_TEST(test_startReporting_fullyPopulated);
    RUN_TEST(test_startReporting_lnCategories_absent_defaultsToAll);
    RUN_TEST(test_startReporting_lnCategories_singleValue);
    RUN_TEST(test_startReporting_lnCategories_allFourValues_orsTogether);
    RUN_TEST(test_startReporting_lnCategories_emptyArray_returnsInvalidParams);
    RUN_TEST(test_startReporting_lnCategories_unrecognizedCategoryName_returnsInvalidParams);
    RUN_TEST(test_startReporting_lnCategories_mixedValidAndInvalid_returnsInvalidParams);
    RUN_TEST(test_startReporting_lnCategories_notAnArray_returnsInvalidParams);
    RUN_TEST(test_startReporting_lnCategories_nonStringElement_returnsInvalidParams);

    RUN_TEST(test_stopReporting_missingDeviceId_returnsInvalidParams);
    RUN_TEST(test_stopReporting_negativeDeviceId_returnsInvalidParams);
    RUN_TEST(test_stopReporting_valid);
    RUN_TEST(test_stopReporting_byAddress_missingHost_returnsInvalidParams);
    RUN_TEST(test_stopReporting_byAddress_emptyHost_returnsInvalidParams);
    RUN_TEST(test_stopReporting_byAddress_valid_appliesDefaultMmsPort);
    RUN_TEST(test_stopReporting_byAddress_valid_explicitMmsPort);
    RUN_TEST(test_stopReporting_deviceIdTakesPrecedenceOverHost_whenBothGiven);

    RUN_TEST(test_startScan_missingInterfaceId_returnsInvalidParams);
    RUN_TEST(test_startScan_emptyInterfaceId_returnsInvalidParams);
    RUN_TEST(test_startScan_minimalValid_appliesDefaults);
    RUN_TEST(test_startScan_fullyPopulated);

    RUN_TEST(test_stopScan_missingScanId_returnsInvalidParams);
    RUN_TEST(test_stopScan_negativeScanId_returnsInvalidParams);
    RUN_TEST(test_stopScan_valid);

    return UNITY_END();
}
