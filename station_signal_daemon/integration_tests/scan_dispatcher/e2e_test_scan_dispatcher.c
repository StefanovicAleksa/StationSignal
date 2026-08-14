#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "cJSON.h"
#include "features/scan_dispatcher/service/scan_dispatcher_api.h"

/*
 * E2E test: starts a real ScanDispatcher (real bind, real libwebsockets
 * service thread), connects a hand-rolled minimal websocket client (raw TCP
 * + HTTP-Upgrade handshake + a small RFC6455 frame parser for the
 * unmasked/unfragmented text frames this server ever sends), drives real
 * ScanDispatcher_publishDeviceFound calls, and asserts the real JSON
 * received over the real socket matches the expected envelope (parsed back
 * with cJSON, not raw string equality). Near-verbatim duplicate of
 * integration_tests/ipc_dispatcher/e2e_test_ipc_dispatcher.c's own test-client
 * helpers - see scan_dispatcher_types.h's own top comment for why this
 * feature is duplicated rather than shared.
 *
 * Deliberately NOT using libwebsockets client mode for the test peer, even
 * though it's already vendored - a bug both client and server share via the
 * same library couldn't be caught this way.
 *
 * Sec-WebSocket-Accept is intentionally NOT verified (would need hand-rolled
 * SHA1 for no benefit to this test's actual goal); the handshake response's
 * "101" status line is enough to know the upgrade succeeded.
 */

#define TEST_PORT 18866
#define TEST_HOST "127.0.0.1"

static ScanDispatcherHandle fixtureHandle;
static int fixtureSocket = -1;

void
setUp(void) {
    fixtureHandle = NULL;
    fixtureSocket = -1;
}

void
tearDown(void) {
    if (fixtureSocket >= 0) {
        close(fixtureSocket);
        fixtureSocket = -1;
    }
    if (fixtureHandle) {
        ScanDispatcher_destroy(fixtureHandle);
        fixtureHandle = NULL;
    }
}

static int
connectAndUpgrade(uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    /* Fixed, syntactically valid 16-byte-base64 Sec-WebSocket-Key (the exact
     * example key from RFC 6455 section 1.3) - a real value is all a server
     * needs to complete the handshake; this test never checks the server's
     * Sec-WebSocket-Accept response (see file header comment). */
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
        return -1;
    }

    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) {
            close(sock);
            return -1;
        }
        total += (size_t) n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    if (!strstr(buf, "101")) {
        close(sock);
        return -1;
    }

    return sock;
}

static bool
recvExact(int sock, uint8_t* out, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(sock, out + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t) r;
    }
    return true;
}

/*
 * Reads exactly one unmasked, unfragmented RFC6455 text frame (the only kind
 * this server ever sends - see scan_dispatcher_ws_server.c) and returns its
 * payload as a heap-allocated, NUL-terminated string. NULL on any framing
 * error or timeout.
 */
static char*
readOneTextFrame(int sock) {
    uint8_t header[2];
    if (!recvExact(sock, header, 2)) return NULL;

    int opcode = header[0] & 0x0F;
    if (opcode != 0x1) return NULL; /* only text frames expected */

    uint64_t payloadLen = header[1] & 0x7F;
    if (payloadLen == 126) {
        uint8_t ext[2];
        if (!recvExact(sock, ext, 2)) return NULL;
        payloadLen = ((uint64_t) ext[0] << 8) | ext[1];
    } else if (payloadLen == 127) {
        uint8_t ext[8];
        if (!recvExact(sock, ext, 8)) return NULL;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i];
    }

    char* payload = malloc(payloadLen + 1);
    if (!payload) return NULL;
    if (payloadLen > 0 && !recvExact(sock, (uint8_t*) payload, payloadLen)) {
        free(payload);
        return NULL;
    }
    payload[payloadLen] = '\0';

    return payload;
}

/* Blocks (with a bounded timeout) until at least one byte is available to
 * read, without consuming it - used so the test doesn't race the producer
 * call's async wake-up + service-thread delivery. */
static bool
waitReadable(int sock, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(sock + 1, &readSet, NULL, NULL, &tv) > 0;
}

/* Host used only by waitUntilCursorLive's probe frames - deliberately not a
 * value any real assertion below matches on. */
#define CURSOR_PROBE_HOST "0.0.0.0"

/*
 * Blocks until this connection's SERVER-SIDE read cursor exists, then leaves
 * the socket drained and ready for the real assertions.
 *
 * WHY THIS IS NEEDED, and why it is not a production bug. connectAndUpgrade
 * returns the moment the client has read the HTTP "101" line, but the server
 * sets that session's ring-buffer cursor later and on a different thread, in
 * LWS_CALLBACK_ESTABLISHED (scan_dispatcher_ws_server.c), as
 * `session->cursor = ScanDispatcherRingBuffer_headSeq(...)` - deliberate
 * start-from-now semantics with no backlog replay. So a publish issued
 * between those two points is appended to the ring, the cursor is then
 * initialized PAST it, and it is never delivered: the frame is dropped with no
 * error anywhere, which is exactly what
 * `test_deviceFound_arrivesAsScanResultJson:FAIL:no frame arrived within
 * timeout` was, and what made test_multipleDeviceFound_arriveInOrder
 * intermittently read 10.0.0.2 as its first frame.
 *
 * Production does not have this hole: scan_orchestration deliberately waits
 * SCAN_ORCHESTRATION_INITIAL_SWEEP_GRACE_MS (300ms) before its first sweep,
 * for precisely this race (see its own CLAUDE.md bullet and CHANGELOG entry).
 * These tests published immediately with no equivalent, so they raced a
 * hazard the real system already handles.
 *
 * The fix is deterministic rather than another sleep: publish a probe frame
 * and see whether it comes back. It cannot come back until the cursor is live,
 * and once one does, the cursor is live by construction - no timing
 * assumption. Every probe that queued up is then drained so the ordering
 * assertions below still see the real frames first.
 */
static bool
waitUntilCursorLive(ScanDispatcherHandle handle, int sock) {
    for (int attempt = 0; attempt < 50; attempt++) {
        ScanDispatcher_publishDeviceFound(handle, 0, CURSOR_PROBE_HOST, 1, false);

        if (waitReadable(sock, 100)) {
            do {
                char* drained = readOneTextFrame(sock);
                if (!drained) return false;
                free(drained);
            } while (waitReadable(sock, 100));
            return true;
        }
    }
    return false;
}

void
test_deviceFound_arrivesAsScanResultJson(void) {
    ScanDispatcherConfig config;
    ScanDispatcherConfig_defaults(&config);
    config.port = TEST_PORT;

    fixtureHandle = ScanDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_OK, ScanDispatcher_start(fixtureHandle));

    fixtureSocket = connectAndUpgrade(TEST_PORT);
    TEST_ASSERT_TRUE_MESSAGE(fixtureSocket >= 0, "websocket handshake failed");
    TEST_ASSERT_TRUE_MESSAGE(waitUntilCursorLive(fixtureHandle, fixtureSocket),
            "the server never delivered a probe frame - its read cursor for this connection never came up");

    ScanDispatcher_publishDeviceFound(fixtureHandle, 1, "127.0.0.1", 102, true);

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "no frame arrived within timeout");
    char* json = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(json);

    cJSON* parsed = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(parsed);

    TEST_ASSERT_TRUE(cJSON_GetObjectItem(parsed, "schemaVersion")->valuedouble == 1.0);
    TEST_ASSERT_EQUAL_STRING("SCAN_RESULT", cJSON_GetObjectItem(parsed, "type")->valuestring);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(parsed, "scanId")->valuedouble == 1.0);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", cJSON_GetObjectItem(parsed, "host")->valuestring);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(parsed, "mmsPort")->valuedouble == 102.0);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(parsed, "discoveredAtMs"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(parsed, "authRequired")));

    cJSON_Delete(parsed);
    free(json);
}

void
test_multipleDeviceFound_arriveInOrder(void) {
    ScanDispatcherConfig config;
    ScanDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 1;

    fixtureHandle = ScanDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_OK, ScanDispatcher_start(fixtureHandle));

    fixtureSocket = connectAndUpgrade(TEST_PORT + 1);
    TEST_ASSERT_TRUE_MESSAGE(fixtureSocket >= 0, "websocket handshake failed");
    TEST_ASSERT_TRUE_MESSAGE(waitUntilCursorLive(fixtureHandle, fixtureSocket),
            "the server never delivered a probe frame - its read cursor for this connection never came up");

    ScanDispatcher_publishDeviceFound(fixtureHandle, 1, "10.0.0.1", 102, false);
    ScanDispatcher_publishDeviceFound(fixtureHandle, 1, "10.0.0.2", 102, false);

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "first frame did not arrive");
    char* first = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(first);
    cJSON* firstParsed = cJSON_Parse(first);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", cJSON_GetObjectItem(firstParsed, "host")->valuestring);
    cJSON_Delete(firstParsed);
    free(first);

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "second frame did not arrive");
    char* second = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(second);
    cJSON* secondParsed = cJSON_Parse(second);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", cJSON_GetObjectItem(secondParsed, "host")->valuestring);
    cJSON_Delete(secondParsed);
    free(second);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_deviceFound_arrivesAsScanResultJson);
    RUN_TEST(test_multipleDeviceFound_arriveInOrder);

    return UNITY_END();
}
