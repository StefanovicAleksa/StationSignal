#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/scl_bootstrap/service/scl_bootstrap_api.h"
#include "hal_thread.h"
#include "sim_types.h"

/*
 * End-to-end test: runs a real "Reporter1" IED simulator (sim_types.h /
 * sim_server.c - see integration_tests/ied_simulator/, fully decoupled from
 * src/) in the same process, with its MMS file services pointed at a real
 * fixture directory (SimServer_setFilestoreBasepath), and drives the real
 * scl_bootstrap service API against it over loopback: TCP-probing a host
 * list, then browsing/downloading the real fixture file over a real MMS
 * association - no mocking of IedConnection/sockets anywhere.
 *
 * Every test in this file runs sequentially (Unity has no parallel test
 * execution) and each creates/starts/stops/destroys its own SimServer -
 * reusing the same TEST_PORT across tests is safe because only one server
 * is ever bound to it at a time.
 */

#define TEST_PORT 10301
/* Deliberately not bound by anything in this file - IedServer_start binds
 * broadly (confirmed empirically: a second loopback address like "127.0.0.2"
 * was ALSO reachable once a server was listening on TEST_PORT, since
 * 127.0.0.0/8 all resolves to the same host and the server doesn't restrict
 * itself to one specific loopback address), so "nothing is listening" can't
 * be modeled with a second loopback address. Used for the single-candidate
 * "no server running at all" test. */
#define DEAD_PORT 10399

/* Also confirmed empirically: this sandboxed environment has no route to
 * this address, so a connect attempt against it doesn't fail fast (no
 * immediate "unreachable") - it hangs until SclBootstrapConfig's
 * tcpProbeTimeoutMs (default 500ms) expires, exactly like a real firewalled/
 * unreachable host on a real network would. Used for the mixed-host-list
 * "one scan, one hit one miss" test. */
#define UNROUTABLE_HOST "10.255.255.1"

#define LIVE_HOST "127.0.0.1"

void
setUp(void) {}

void
tearDown(void) {}

static uint8_t*
readWholeFile(const char* path, uint32_t* outSize) {
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "expected fixture file to exist on disk");

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* buffer = malloc((size_t) size);
    TEST_ASSERT_NOT_NULL(buffer);

    size_t readBytes = fread(buffer, 1, (size_t) size, f);
    TEST_ASSERT_EQUAL_UINT((size_t) size, readBytes);
    fclose(f);

    *outSize = (uint32_t) size;
    return buffer;
}

static LinkedList
makeHostList(const char* host) {
    LinkedList list = LinkedList_create();
    LinkedList_add(list, (void*) host);
    return list;
}

static LinkedList
makeHostListTwo(const char* host1, const char* host2) {
    LinkedList list = LinkedList_create();
    LinkedList_add(list, (void*) host1);
    LinkedList_add(list, (void*) host2);
    return list;
}

static SclBootstrapResult*
resultAt(LinkedList results, int index) {
    LinkedList element = LinkedList_get(results, index);
    return element ? (SclBootstrapResult*) LinkedList_getData(element) : NULL;
}

void
test_scan_findsLiveServerAndSkipsUnreachableOne(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/served_files/");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    LinkedList hosts = makeHostListTwo(LIVE_HOST, UNROUTABLE_HOST);

    SclBootstrapError err;
    SclBootstrapHandle handle = SclBootstrap_create(NULL, &err);
    TEST_ASSERT_NOT_NULL(handle);

    LinkedList results = SclBootstrap_scanAndFetch(handle, hosts, TEST_PORT, &err);
    TEST_ASSERT_NOT_NULL(results);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(results));

    SclBootstrapResult* liveResult = resultAt(results, 0);
    TEST_ASSERT_NOT_NULL(liveResult);
    TEST_ASSERT_EQUAL_STRING(LIVE_HOST, liveResult->host);
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED, liveResult->status);
    TEST_ASSERT_EQUAL_STRING("reporter1.cid", liveResult->fileName);

    uint32_t expectedSize = 0;
    uint8_t* expected = readWholeFile("fixtures/served_files/reporter1.cid", &expectedSize);
    TEST_ASSERT_EQUAL_UINT32(expectedSize, liveResult->fileSize);
    TEST_ASSERT_EQUAL_MEMORY(expected, liveResult->fileData, expectedSize);
    free(expected);

    SclBootstrapResult* unreachableResult = resultAt(results, 1);
    TEST_ASSERT_NOT_NULL(unreachableResult);
    TEST_ASSERT_EQUAL_STRING(UNROUTABLE_HOST, unreachableResult->host);
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER, unreachableResult->status);

    LinkedList_destroyDeep(results, SclBootstrap_destroyResult);
    SclBootstrap_destroy(handle);
    LinkedList_destroyStatic(hosts);

    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_scan_noServerListening_reportsNoMmsServer(void) {
    LinkedList hosts = makeHostList(LIVE_HOST);

    SclBootstrapError err;
    SclBootstrapHandle handle = SclBootstrap_create(NULL, &err);
    LinkedList results = SclBootstrap_scanAndFetch(handle, hosts, DEAD_PORT, &err);

    SclBootstrapResult* result = resultAt(results, 0);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER, result->status);

    LinkedList_destroyDeep(results, SclBootstrap_destroyResult);
    SclBootstrap_destroy(handle);
    LinkedList_destroyStatic(hosts);
}

void
test_scan_noSclFilePresent_reportsNotFound(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/no_scl_files/");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    LinkedList hosts = makeHostList(LIVE_HOST);

    SclBootstrapError err;
    SclBootstrapHandle handle = SclBootstrap_create(NULL, &err);
    LinkedList results = SclBootstrap_scanAndFetch(handle, hosts, TEST_PORT, &err);

    SclBootstrapResult* result = resultAt(results, 0);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND, result->status);

    LinkedList_destroyDeep(results, SclBootstrap_destroyResult);
    SclBootstrap_destroy(handle);
    LinkedList_destroyStatic(hosts);

    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_scan_authRequiredNoPasswordConfigured_deniesAccess(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/auth_required/");
    SimServer_requireAuthentication(sim, "secret123");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    LinkedList hosts = makeHostList(LIVE_HOST);

    SclBootstrapError err;
    SclBootstrapHandle handle = SclBootstrap_create(NULL, &err); /* no acseAuthPassword configured */
    LinkedList results = SclBootstrap_scanAndFetch(handle, hosts, TEST_PORT, &err);

    SclBootstrapResult* result = resultAt(results, 0);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED, result->status);
    TEST_ASSERT_FALSE_MESSAGE(result->authWasAttempted,
            "no password was configured, so no auth retry should have been attempted");

    LinkedList_destroyDeep(results, SclBootstrap_destroyResult);
    SclBootstrap_destroy(handle);
    LinkedList_destroyStatic(hosts);

    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_scan_authRequiredCorrectPassword_retrievesFile(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/auth_required/");
    SimServer_requireAuthentication(sim, "secret123");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    LinkedList hosts = makeHostList(LIVE_HOST);

    SclBootstrapConfig config;
    SclBootstrapConfig_defaults(&config);
    config.acseAuthPassword = "secret123";

    SclBootstrapError err;
    SclBootstrapHandle handle = SclBootstrap_create(&config, &err);
    LinkedList results = SclBootstrap_scanAndFetch(handle, hosts, TEST_PORT, &err);

    SclBootstrapResult* result = resultAt(results, 0);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED, result->status);
    TEST_ASSERT_TRUE_MESSAGE(result->authWasAttempted,
            "the unauthenticated first attempt should have been rejected, triggering the auth retry");
    TEST_ASSERT_EQUAL_STRING("reporter1.cid", result->fileName);

    LinkedList_destroyDeep(results, SclBootstrap_destroyResult);
    SclBootstrap_destroy(handle);
    LinkedList_destroyStatic(hosts);

    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_scan_authRequiredWrongPassword_deniesAccess(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/auth_required/");
    SimServer_requireAuthentication(sim, "secret123");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    LinkedList hosts = makeHostList(LIVE_HOST);

    SclBootstrapConfig config;
    SclBootstrapConfig_defaults(&config);
    config.acseAuthPassword = "wrong-password";

    SclBootstrapError err;
    SclBootstrapHandle handle = SclBootstrap_create(&config, &err);
    LinkedList results = SclBootstrap_scanAndFetch(handle, hosts, TEST_PORT, &err);

    SclBootstrapResult* result = resultAt(results, 0);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED, result->status);
    TEST_ASSERT_TRUE(result->authWasAttempted);

    LinkedList_destroyDeep(results, SclBootstrap_destroyResult);
    SclBootstrap_destroy(handle);
    LinkedList_destroyStatic(hosts);

    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_scan_findsLiveServerAndSkipsUnreachableOne);
    RUN_TEST(test_scan_noServerListening_reportsNoMmsServer);
    RUN_TEST(test_scan_noSclFilePresent_reportsNotFound);
    RUN_TEST(test_scan_authRequiredNoPasswordConfigured_deniesAccess);
    RUN_TEST(test_scan_authRequiredCorrectPassword_retrievesFile);
    RUN_TEST(test_scan_authRequiredWrongPassword_deniesAccess);

    return UNITY_END();
}
