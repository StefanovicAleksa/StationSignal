#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "hal_thread.h"
#include "scan_orchestration/service/scan_orchestration_api.h"

/*
 * E2E test: drives the REAL ScanOrchestration_startScan/_stopScan against
 * the real "lo" interface with two different mmsPorts, proving the
 * refcounted shared-websocket sequencing end to end. Sweeps against "lo" are
 * EXPECTED to fail with IED_DISCOVERY_ERR_SUBNET_TOO_LARGE (see this
 * directory's own Makefile comment) - this test proves sequencing/
 * refcounting/threading, not sweep success.
 *
 * Liveness of the shared dispatcher's websocket is probed with a
 * hand-rolled minimal RFC6455 handshake (connect + HTTP Upgrade, same
 * helper shape as integration_tests/scan_dispatcher/'s own test client) -
 * a successful "101" handshake means the port is bound and serving; a
 * failed connect/handshake means it's torn down.
 */

#define TEST_PORT 18867
#define TEST_HOST "127.0.0.1"

static ScanOrchestrationHandle fixtureHandle;

void
setUp(void) {
    fixtureHandle = NULL;
}

void
tearDown(void) {
    if (fixtureHandle) {
        ScanOrchestration_destroy(fixtureHandle);
        fixtureHandle = NULL;
    }
}

static ScanOrchestrationHandle
createHandleOnTestPort(void) {
    ScanOrchestrationConfig config;
    ScanOrchestrationConfig_defaults(&config);
    config.scanDispatcherConfig.port = TEST_PORT;
    config.defaultSweepIntervalMs = 50; /* fast sweeps, keeps the test quick */
    return ScanOrchestration_create(&config, NULL);
}

/* Returns true if the dispatcher's websocket at TEST_PORT is currently bound
 * and serving (a real connect + HTTP Upgrade handshake completes with a 101
 * response), false if the port is unbound/refused. Closes its own socket
 * before returning either way - this is a liveness probe, not a persistent
 * client connection. */
static bool
dispatcherIsServing(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
        close(sock);
        return false;
    }

    const char* request =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    if (send(sock, request, strlen(request), 0) < 0) {
        close(sock);
        return false;
    }

    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) {
            close(sock);
            return false;
        }
        total += (size_t) n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    close(sock);
    return strstr(buf, "101") != NULL;
}

void
test_fullRefcountedLifecycle_overLoopback(void) {
    fixtureHandle = createHandleOnTestPort();
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    ScanRequest requestA = { .interfaceId = "lo", .mmsPort = 102, .sweepIntervalMs = 0, .acseAuthPassword = NULL };
    ScanRequest requestB = { .interfaceId = "lo", .mmsPort = 103, .sweepIntervalMs = 0, .acseAuthPassword = NULL };

    /* Before any scan, nothing is listening. */
    TEST_ASSERT_FALSE_MESSAGE(dispatcherIsServing(), "dispatcher must not be bound before any scan starts");

    /* 1. Starting scan #1 (0->1) binds the shared dispatcher. */
    uint64_t scanIdA;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &requestA, &scanIdA));
    TEST_ASSERT_TRUE_MESSAGE(dispatcherIsServing(), "dispatcher must be serving after the first scan starts");

    /* 2. Starting scan #2 (different mmsPort, same interface) must NOT fail
     *    or rebind - it shares the same already-running dispatcher (1->2). */
    uint64_t scanIdB;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &requestB, &scanIdB));
    TEST_ASSERT_TRUE(scanIdA != scanIdB);
    TEST_ASSERT_TRUE_MESSAGE(dispatcherIsServing(), "dispatcher must still be serving with two active scans");

    /* 3. Stopping scan #1 (2->1) must leave the dispatcher running - scan #2 is still active. */
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, scanIdA));
    TEST_ASSERT_TRUE_MESSAGE(dispatcherIsServing(), "dispatcher must stay up while scan #2 is still active");

    /* 4. Stopping scan #2, the last one (1->0), tears the dispatcher down. */
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, scanIdB));
    TEST_ASSERT_FALSE_MESSAGE(dispatcherIsServing(), "dispatcher must be torn down once the last scan stops");

    /* 5. A subsequent scan must cleanly rebind the exact same port - mirrors
     *    ipc_dispatcher's own proven start-after-stop guarantee. */
    uint64_t scanIdC;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &requestA, &scanIdC));
    TEST_ASSERT_TRUE_MESSAGE(dispatcherIsServing(), "dispatcher must cleanly rebind the same port for a 3rd scan");
    TEST_ASSERT_TRUE(scanIdC != scanIdA && scanIdC != scanIdB);

    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, scanIdC));
    TEST_ASSERT_FALSE(dispatcherIsServing());
}

void
test_stopScan_genuinelyBlocksUntilWorkerThreadExits(void) {
    fixtureHandle = createHandleOnTestPort();
    ScanRequest request = { .interfaceId = "lo", .mmsPort = 102, .sweepIntervalMs = 0, .acseAuthPassword = NULL };

    uint64_t scanId;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &request, &scanId));

    /* Let at least one sweep pass happen first. */
    Thread_sleep(100);

    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, scanId));

    /* If stopScan returned without the worker thread having genuinely
     * exited, the dispatcher would still show as serving (or the process
     * could later crash on a dangling thread) - this is the same
     * "provably stopped, not just marked stopped" assertion
     * test_startStopStart_reusesPortCleanly-style tests already rely on. */
    TEST_ASSERT_FALSE(dispatcherIsServing());
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_fullRefcountedLifecycle_overLoopback);
    RUN_TEST(test_stopScan_genuinelyBlocksUntilWorkerThreadExits);

    return UNITY_END();
}
