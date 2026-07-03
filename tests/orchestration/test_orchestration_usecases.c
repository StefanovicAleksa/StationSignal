#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "linked_list.h"
#include "orchestration/domain/orchestration_usecases.h"

void
setUp(void) {}

void
tearDown(void) {}

static SclBootstrapResult*
makeResult(const char* host, SclBootstrapCandidateStatus status) {
    SclBootstrapResult* result = calloc(1, sizeof(SclBootstrapResult));
    result->host = strdup(host);
    result->port = 102;
    result->status = status;
    result->lastMmsError = IED_ERROR_OK;
    return result;
}

/* Local equivalent of SclBootstrap_destroyResult - avoids pulling the whole
 * scl_bootstrap_api.c (and its data-layer dependencies) into this test binary
 * just to free a hand-built struct; this test stays pure logic over
 * SclBootstrapResult/LinkedList, matching this file's own no-libiec61850-
 * symbols-beyond-LinkedList posture. */
static void
freeResult(void* resultPtr) {
    SclBootstrapResult* result = (SclBootstrapResult*) resultPtr;
    if (!result) return;
    free(result->host);
    free(result->fileName);
    free(result->fileData);
    free(result);
}

/* ---- OrchestrationUseCases_selectAndDetachFirstRetrieved ---- */

void
test_selectAndDetach_returnsNull_whenNoneRetrieved(void) {
    LinkedList results = LinkedList_create();
    LinkedList_add(results, makeResult("10.0.0.1", SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER));
    LinkedList_add(results, makeResult("10.0.0.2", SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED));

    SclBootstrapResult* winner = OrchestrationUseCases_selectAndDetachFirstRetrieved(results);

    TEST_ASSERT_NULL(winner);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(results));

    LinkedList_destroyDeep(results, freeResult);
}

void
test_selectAndDetach_picksFirstRetrieved_andDetachesIt(void) {
    SclBootstrapResult* expected = makeResult("10.0.0.2", SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED);

    LinkedList results = LinkedList_create();
    LinkedList_add(results, makeResult("10.0.0.1", SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER));
    LinkedList_add(results, expected);
    LinkedList_add(results, makeResult("10.0.0.3", SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED));

    SclBootstrapResult* winner = OrchestrationUseCases_selectAndDetachFirstRetrieved(results);

    TEST_ASSERT_TRUE(winner == expected);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", winner->host);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(results));

    LinkedList_destroyDeep(results, freeResult);
    freeResult(winner);
}

void
test_selectAndDetach_returnsNull_onEmptyOrNullList(void) {
    LinkedList empty = LinkedList_create();
    TEST_ASSERT_NULL(OrchestrationUseCases_selectAndDetachFirstRetrieved(empty));
    LinkedList_destroy(empty);

    TEST_ASSERT_NULL(OrchestrationUseCases_selectAndDetachFirstRetrieved(NULL));
}

/* ---- OrchestrationUseCases_summarizeBootstrapFailure ---- */

void
test_summarizeFailure_returnsLastElementStatus(void) {
    LinkedList results = LinkedList_create();
    LinkedList_add(results, makeResult("10.0.0.1", SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER));
    LinkedList_add(results, makeResult("10.0.0.2", SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED));

    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED,
            OrchestrationUseCases_summarizeBootstrapFailure(results));

    LinkedList_destroyDeep(results, freeResult);
}

void
test_summarizeFailure_returnsNoMmsServer_whenEmptyOrNull(void) {
    LinkedList empty = LinkedList_create();
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER,
            OrchestrationUseCases_summarizeBootstrapFailure(empty));
    LinkedList_destroy(empty);

    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER,
            OrchestrationUseCases_summarizeBootstrapFailure(NULL));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_selectAndDetach_returnsNull_whenNoneRetrieved);
    RUN_TEST(test_selectAndDetach_picksFirstRetrieved_andDetachesIt);
    RUN_TEST(test_selectAndDetach_returnsNull_onEmptyOrNullList);

    RUN_TEST(test_summarizeFailure_returnsLastElementStatus);
    RUN_TEST(test_summarizeFailure_returnsNoMmsServer_whenEmptyOrNull);

    return UNITY_END();
}
