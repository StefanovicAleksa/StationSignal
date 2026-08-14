#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "hal_thread.h"
#include "cJSON.h"
#include "features/ipc_dispatcher/service/ipc_dispatcher_api.h"

/*
 * E2E test: starts a real IpcDispatcher (real bind, real libwebsockets
 * service thread), connects a hand-rolled minimal websocket client (raw TCP
 * + HTTP-Upgrade handshake + a small RFC6455 frame parser for the
 * unmasked/unfragmented text frames this server ever sends), drives real
 * MmsReportRecord/GooseSubscriberRecord fixtures through
 * IpcDispatcher_onMmsReport/_onGooseRecord, and asserts the real JSON
 * received over the real socket matches the expected envelope (parsed back
 * with cJSON, not raw string equality - resilient to key ordering).
 *
 * Deliberately NOT using libwebsockets client mode for the test peer, even
 * though it's already vendored - a bug both client and server share via the
 * same library couldn't be caught this way. Same "keep the fake peer
 * decoupled from the code under test" philosophy integration_tests/
 * ied_simulator/ already applies at the protocol level (zero src/ includes
 * there; here, zero libwebsockets includes in the test client path).
 *
 * Sec-WebSocket-Accept is intentionally NOT verified (would need hand-rolled
 * SHA1 for no benefit to this test's actual goal - proving message
 * delivery, not protocol-compliance certification); the handshake response's
 * "101" status line is enough to know the upgrade succeeded.
 */

#define TEST_PORT 18770
#define TEST_HOST "127.0.0.1"

static IpcDispatcherHandle fixtureHandle;
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
        IpcDispatcher_destroy(fixtureHandle);
        fixtureHandle = NULL;
    }
}

static int
connectAndUpgrade(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
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

    /* Read until the end of the HTTP response header block. */
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
 * this server ever sends - see ipc_dispatcher_ws_server.c) and returns its
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
 * callback's async wake-up + service-thread delivery. */
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

/*
 * Blocks until this connection's SERVER-SIDE read cursor exists, then leaves
 * the socket drained. Identical in purpose and mechanism to
 * integration_tests/scan_dispatcher's own helper of the same name - see that
 * one's full comment for the race and why it is a test hazard rather than a
 * production bug.
 *
 * In short: connectAndUpgrade returns once the client has read the HTTP "101",
 * but the server initializes this session's cursor later and on another thread
 * (LWS_CALLBACK_ESTABLISHED, `session->cursor = ...headSeq(...)`, deliberate
 * start-from-now with no backlog replay), so anything published in between is
 * appended and then skipped, silently.
 *
 * This suite has not been observed failing on it, but only because each test
 * happens to spend ~20 lines building an MmsReportRecord/GooseSubscriberRecord
 * between the upgrade and the publish, which incidentally gives the service
 * thread time to run. That is luck, not design - scan_dispatcher's copy of
 * these same helpers, which publishes immediately, failed roughly three runs
 * in four. Made explicit here so this suite does not start flaking the first
 * time someone shortens the setup.
 *
 * CONNECTION_STATUS is used as the probe because it is the cheapest frame this
 * dispatcher can be made to emit (no record to build or hand ownership of) and
 * because no test in this suite asserts on that type.
 */
static bool
waitUntilCursorLive(IpcDispatcherHandle handle, int sock) {
    for (int attempt = 0; attempt < 50; attempt++) {
        IpcDispatcher_onConnStateChange(handle, MMS_REPORT_CLIENT_CONNECTION_REJECTED);

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
test_mmsReport_withStValAndQ_arrivesAsPairedDataPoint(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);
    config.port = TEST_PORT;

    fixtureHandle = IpcDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_OK, IpcDispatcher_start(fixtureHandle));

    fixtureSocket = connectAndUpgrade();
    TEST_ASSERT_TRUE_MESSAGE(fixtureSocket >= 0, "websocket handshake failed");
    TEST_ASSERT_TRUE_MESSAGE(waitUntilCursorLive(fixtureHandle, fixtureSocket),
            "the server never delivered a probe frame - its read cursor for this connection never came up");

    MmsReportRecord* record = calloc(1, sizeof(MmsReportRecord));
    record->rcbReference = strdup("Reporter1LD1/LLN0.BR.brcbMain");
    record->buffered = true;
    record->rptId = strdup("rpt1");
    record->hasTimestamp = true;
    record->timestampMs = 1751520000123ULL;
    record->entryCount = 2;
    record->entries = calloc(2, sizeof(MmsReportEntry));
    record->entries[0].reference = strdup("Reporter1LD1/LLN0$ST$Ind1$stVal");
    record->entries[0].value = MmsValue_newBoolean(true);
    record->entries[0].previousValue = MmsValue_newBoolean(false);
    record->entries[0].ownChangeDetected = true; /* false -> true: a genuine own change */
    record->entries[1].reference = strdup("Reporter1LD1/LLN0$ST$Ind1$q");
    MmsValue* quality = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(quality, QUALITY_VALIDITY_GOOD);
    record->entries[1].value = quality;
    MmsValue* previousQuality = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(previousQuality, QUALITY_VALIDITY_INVALID);
    record->entries[1].previousValue = previousQuality;
    record->entries[1].ownChangeDetected = true; /* INVALID -> GOOD: a genuine own change */

    IpcDispatcher_onMmsReport(fixtureHandle, record); /* ownership transferred */

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "no frame arrived within timeout");
    char* json = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(json);

    cJSON* parsed = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(parsed);

    TEST_ASSERT_EQUAL_STRING("MMS_REPORT", cJSON_GetObjectItem(parsed, "type")->valuestring);

    cJSON* source = cJSON_GetObjectItem(parsed, "source");
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/LLN0.BR.brcbMain", cJSON_GetObjectItem(source, "rcbReference")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(source, "buffered")));

    cJSON* dataPoints = cJSON_GetObjectItem(parsed, "dataPoints");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(dataPoints)); /* stVal+q merged into one point */

    cJSON* point = cJSON_GetArrayItem(dataPoints, 0);
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/LLN0$ST$Ind1$stVal", cJSON_GetObjectItem(point, "reference")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(point, "value")));

    cJSON* qualityJson = cJSON_GetObjectItem(point, "quality");
    TEST_ASSERT_FALSE(cJSON_IsNull(qualityJson));
    TEST_ASSERT_EQUAL_STRING("GOOD", cJSON_GetObjectItem(qualityJson, "validity")->valuestring);

    cJSON* previousValueJson = cJSON_GetObjectItem(point, "previousValue");
    TEST_ASSERT_FALSE(cJSON_IsNull(previousValueJson));
    TEST_ASSERT_FALSE(cJSON_IsTrue(previousValueJson));

    cJSON* previousQualityJson = cJSON_GetObjectItem(point, "previousQuality");
    TEST_ASSERT_FALSE(cJSON_IsNull(previousQualityJson));
    TEST_ASSERT_EQUAL_STRING("INVALID", cJSON_GetObjectItem(previousQualityJson, "validity")->valuestring);

    cJSON_Delete(parsed);
    free(json);
}

void
test_gooseRecord_arrivesWithGoCbRefSource(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 1;

    fixtureHandle = IpcDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_OK, IpcDispatcher_start(fixtureHandle));

    /* connectAndUpgrade() always targets TEST_PORT - reconnect against this
     * test's own port by opening the socket manually via the same helper's
     * logic at TEST_PORT + 1. */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(sock >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 1);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    TEST_ASSERT_EQUAL_INT(0, connect(sock, (struct sockaddr*) &addr, sizeof(addr)));

    const char* request =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    send(sock, request, strlen(request), 0);

    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        TEST_ASSERT_TRUE(n > 0);
        total += (size_t) n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    TEST_ASSERT_NOT_NULL(strstr(buf, "101"));
    fixtureSocket = sock;
    TEST_ASSERT_TRUE_MESSAGE(waitUntilCursorLive(fixtureHandle, fixtureSocket),
            "the server never delivered a probe frame - its read cursor for this connection never came up");

    GooseSubscriberRecord* record = calloc(1, sizeof(GooseSubscriberRecord));
    record->goCbRef = strdup("Breaker1CB1/LLN0$GO$gcbStatus");
    record->timestampMs = 1751520000456ULL;
    record->entryCount = 1;
    record->entries = calloc(1, sizeof(GooseSubscriberEntry));
    record->entries[0].reference = strdup("Breaker1CB1/LLN0$ST$Pos$stVal");
    record->entries[0].value = MmsValue_newIntegerFromInt32(2);

    IpcDispatcher_onGooseRecord(fixtureHandle, record); /* ownership transferred */

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "no frame arrived within timeout");
    char* json = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(json);

    cJSON* parsed = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(parsed);

    TEST_ASSERT_EQUAL_STRING("GOOSE", cJSON_GetObjectItem(parsed, "type")->valuestring);

    cJSON* source = cJSON_GetObjectItem(parsed, "source");
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/LLN0$GO$gcbStatus", cJSON_GetObjectItem(source, "goCbRef")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(source, "buffered")); /* GOOSE never carries this field */

    cJSON* dataPoints = cJSON_GetObjectItem(parsed, "dataPoints");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(dataPoints));
    cJSON* point = cJSON_GetArrayItem(dataPoints, 0);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/LLN0$ST$Pos$stVal", cJSON_GetObjectItem(point, "reference")->valuestring);

    cJSON_Delete(parsed);
    free(json);
}

/*
 * Proves IpcDispatcherUseCases_shouldForwardWithinProtocol end-to-end for MMS:
 * two different RCBs (rcbA, rcbB) each independently report the exact same
 * (reference, value, quality) content - the daemon-side redundant-coverage
 * signature this whole cache exists to catch (see mms_dataset_manager's own
 * fix for the actual root cause; this is the last-resort delivery-side net).
 * Only the FIRST push's data point must reach the client; the second push's
 * only entry is fully suppressed, so its own envelope arrives with an EMPTY
 * dataPoints array (same shape as any other "nothing survived filtering"
 * push - not a missing message, just a content-free one).
 */
void
test_mmsReport_duplicateFromDifferentRcb_isSuppressed(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 2;

    fixtureHandle = IpcDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_OK, IpcDispatcher_start(fixtureHandle));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(sock >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 2);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    TEST_ASSERT_EQUAL_INT(0, connect(sock, (struct sockaddr*) &addr, sizeof(addr)));
    const char* request =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    send(sock, request, strlen(request), 0);
    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        TEST_ASSERT_TRUE(n > 0);
        total += (size_t) n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    TEST_ASSERT_NOT_NULL(strstr(buf, "101"));
    fixtureSocket = sock;
    TEST_ASSERT_TRUE_MESSAGE(waitUntilCursorLive(fixtureHandle, fixtureSocket),
            "the server never delivered a probe frame - its read cursor for this connection never came up");

    MmsReportRecord* recordA = calloc(1, sizeof(MmsReportRecord));
    recordA->rcbReference = strdup("Reporter1LD1/LLN0.BR.rcbA");
    recordA->rptId = strdup("rptA");
    recordA->hasTimestamp = true;
    recordA->timestampMs = 1751520000000ULL;
    recordA->entryCount = 1;
    recordA->entries = calloc(1, sizeof(MmsReportEntry));
    recordA->entries[0].reference = strdup("Reporter1LD1/LLN0$ST$Ind1$stVal");
    recordA->entries[0].value = MmsValue_newBoolean(true);
    recordA->entries[0].ownChangeDetected = true;

    IpcDispatcher_onMmsReport(fixtureHandle, recordA);

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "no frame arrived for recordA within timeout");
    char* jsonA = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(jsonA);
    cJSON* parsedA = cJSON_Parse(jsonA);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cJSON_GetArraySize(cJSON_GetObjectItem(parsedA, "dataPoints")),
            "the first RCB's report must forward its data point normally");
    cJSON_Delete(parsedA);
    free(jsonA);

    /* Different RCB, IDENTICAL (reference, value) content, no quality on
     * either side - the daemon-side cross-RCB duplicate this cache exists to
     * catch. */
    MmsReportRecord* recordB = calloc(1, sizeof(MmsReportRecord));
    recordB->rcbReference = strdup("Reporter1LD1/LLN0.BR.rcbB");
    recordB->rptId = strdup("rptB");
    recordB->hasTimestamp = true;
    recordB->timestampMs = 1751520000001ULL;
    recordB->entryCount = 1;
    recordB->entries = calloc(1, sizeof(MmsReportEntry));
    recordB->entries[0].reference = strdup("Reporter1LD1/LLN0$ST$Ind1$stVal");
    recordB->entries[0].value = MmsValue_newBoolean(true);
    recordB->entries[0].ownChangeDetected = true;

    IpcDispatcher_onMmsReport(fixtureHandle, recordB);

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "no frame arrived for recordB within timeout");
    char* jsonB = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(jsonB);
    cJSON* parsedB = cJSON_Parse(jsonB);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cJSON_GetArraySize(cJSON_GetObjectItem(parsedB, "dataPoints")),
            "a different RCB reporting the exact same content must be suppressed as a cross-RCB duplicate");
    cJSON_Delete(parsedB);
    free(jsonB);
}

/*
 * Proves MMS and GOOSE are deliberately NEVER cross-checked against each
 * other (see IpcDispatcherDedupCache's own doc comment, ipc_dispatcher_types.h) -
 * an MMS RCB and a GOOSE GoCB legitimately wired to the same point are two
 * independent wire paths, and a discrepancy between them is diagnostically
 * useful, so both must always reach the client even with byte-identical
 * (reference, value) content.
 */
void
test_mmsReportAndGooseRecord_sameContent_bothArrive(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 3;

    fixtureHandle = IpcDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_OK, IpcDispatcher_start(fixtureHandle));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(sock >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 3);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    TEST_ASSERT_EQUAL_INT(0, connect(sock, (struct sockaddr*) &addr, sizeof(addr)));
    const char* request =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    send(sock, request, strlen(request), 0);
    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        TEST_ASSERT_TRUE(n > 0);
        total += (size_t) n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    TEST_ASSERT_NOT_NULL(strstr(buf, "101"));
    fixtureSocket = sock;
    TEST_ASSERT_TRUE_MESSAGE(waitUntilCursorLive(fixtureHandle, fixtureSocket),
            "the server never delivered a probe frame - its read cursor for this connection never came up");

    MmsReportRecord* mmsRecord = calloc(1, sizeof(MmsReportRecord));
    mmsRecord->rcbReference = strdup("Reporter1LD1/LLN0.BR.brcbMain");
    mmsRecord->rptId = strdup("rpt1");
    mmsRecord->hasTimestamp = true;
    mmsRecord->timestampMs = 1751520000000ULL;
    mmsRecord->entryCount = 1;
    mmsRecord->entries = calloc(1, sizeof(MmsReportEntry));
    mmsRecord->entries[0].reference = strdup("Breaker1CB1/LLN0$ST$Pos$stVal");
    mmsRecord->entries[0].value = MmsValue_newIntegerFromInt32(2);
    mmsRecord->entries[0].ownChangeDetected = true;

    IpcDispatcher_onMmsReport(fixtureHandle, mmsRecord);

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "no frame arrived for the MMS report within timeout");
    char* mmsJson = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(mmsJson);
    cJSON* mmsParsed = cJSON_Parse(mmsJson);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cJSON_GetArraySize(cJSON_GetObjectItem(mmsParsed, "dataPoints")),
            "the MMS report must forward its data point normally");
    cJSON_Delete(mmsParsed);
    free(mmsJson);

    /* A GOOSE record over a DIFFERENT protocol, byte-identical (reference,
     * value) content - must NOT be suppressed, unlike the same scenario
     * within one protocol (see test_mmsReport_duplicateFromDifferentRcb_isSuppressed
     * above). */
    GooseSubscriberRecord* gooseRecord = calloc(1, sizeof(GooseSubscriberRecord));
    gooseRecord->goCbRef = strdup("Breaker1CB1/LLN0$GO$gcbStatus");
    gooseRecord->timestampMs = 1751520000001ULL;
    gooseRecord->entryCount = 1;
    gooseRecord->entries = calloc(1, sizeof(GooseSubscriberEntry));
    gooseRecord->entries[0].reference = strdup("Breaker1CB1/LLN0$ST$Pos$stVal");
    gooseRecord->entries[0].value = MmsValue_newIntegerFromInt32(2);

    IpcDispatcher_onGooseRecord(fixtureHandle, gooseRecord);

    TEST_ASSERT_TRUE_MESSAGE(waitReadable(fixtureSocket, 3000), "no frame arrived for the GOOSE record within timeout");
    char* gooseJson = readOneTextFrame(fixtureSocket);
    TEST_ASSERT_NOT_NULL(gooseJson);
    cJSON* gooseParsed = cJSON_Parse(gooseJson);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cJSON_GetArraySize(cJSON_GetObjectItem(gooseParsed, "dataPoints")),
            "the GOOSE record must still forward its data point despite matching a just-forwarded "
            "MMS report's content - MMS and GOOSE are never cross-checked against each other");
    cJSON_Delete(gooseParsed);
    free(gooseJson);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_mmsReport_withStValAndQ_arrivesAsPairedDataPoint);
    RUN_TEST(test_gooseRecord_arrivesWithGoCbRefSource);
    RUN_TEST(test_mmsReport_duplicateFromDifferentRcb_isSuppressed);
    RUN_TEST(test_mmsReportAndGooseRecord_sameContent_bothArrive);

    return UNITY_END();
}
