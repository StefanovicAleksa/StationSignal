#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "device_manager/service/device_manager_api.h"
#include "hal_thread.h"
#include "hal_time.h"
#include "cJSON.h"
#include "sim_types.h"

/*
 * E2E test: drives the REAL DeviceManager_startReporting/_stopReporting
 * against TWO real "Reporter1" IED simulator instances (sim_types.h/
 * sim_server.c, see integration_tests/ied_simulator/, fully decoupled from
 * src/) running in this same process, over loopback, at two different
 * mmsPorts - proving:
 *   1. Two concurrent StartReporting calls (driven from two threads) don't
 *      serialize behind each other's slow phase (the two-phase-locked
 *      registry's whole reason for existing - see
 *      src/device_manager/data/device_manager_registry.h's own top
 *      comment). Timing bound is deliberately coarse/non-flaky - the
 *      registry's own unit tests already prove the lock is structurally
 *      released before the slow call; this just proves it holds up against
 *      two REAL bootstrap+MMS+GOOSE sequences too.
 *   2. Each device gets a distinct deviceId and a distinct, real,
 *      independently-connectable ipc_dispatcher websocket, each streaming
 *      real report/GOOSE JSON for its own simulator.
 *   3. Stopping one device leaves the other running; stopping the second
 *      frees its port for reuse by a subsequent start.
 *
 * REQUIRES CAP_NET_RAW (raw AF_PACKET socket) - inherited transitively from
 * the GOOSE subscriber step every DeviceManager_startReporting call reaches
 * via orchestration - run with sudo:
 *   sudo make run
 */

#define TEST_PORT_A 10501
#define TEST_PORT_B 10502
#define TEST_INTERFACE "lo"
#define IED_NAME "Reporter1"
#define DM_WS_PORT_RANGE_START 19500
#define DM_WS_PORT_RANGE_END 19599
#define EXPECTED_GOCB_REF "Reporter1LD1/LLN0$GO$gcbInd"
#define POLL_INTERVAL_MS 100
#define POLL_MAX_ATTEMPTS 100 /* 100 * 100ms = 10s bound */

typedef struct {
    DeviceManagerHandle deviceManager;
    const char* host;
    int mmsPort;
    volatile bool done;
    uint64_t deviceId;
    uint16_t wsPort;
    DeviceManagerError result;
    DeviceManagerErrorDetail detail;
} StartTask;

static void*
startTaskRun(void* param) {
    StartTask* task = (StartTask*) param;
    task->result = DeviceManager_startReporting(task->deviceManager, task->host, task->mmsPort, IED_NAME,
            TEST_INTERFACE, NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,&task->deviceId, &task->wsPort,
            &task->detail, NULL, NULL);
    task->done = true;
    return NULL;
}

static bool
waitForTask(volatile bool* done) {
    for (int i = 0; i < POLL_MAX_ATTEMPTS; i++) {
        if (*done) return true;
        Thread_sleep(POLL_INTERVAL_MS);
    }
    return false;
}

/* ---- hand-rolled minimal websocket test client (same shape as
 * integration_tests/orchestration/e2e_test_orchestration.c's own) ---- */

static int
connectAndUpgrade(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
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

static char*
readOneTextFrame(int sock) {
    uint8_t header[2];
    if (!recvExact(sock, header, 2)) return NULL;

    int opcode = header[0] & 0x0F;
    if (opcode != 0x1) return NULL;

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

/* Reads frames off `ws` until a GOOSE JSON message with the expected goCbRef
 * arrives (proving this specific device's own report/GOOSE feed is live),
 * or the bounded attempt count is exhausted. */
static bool
waitForGooseFrame(int ws) {
    for (int i = 0; i < 15; i++) {
        if (!waitReadable(ws, 2000)) return false;
        char* json = readOneTextFrame(ws);
        if (!json) return false;

        cJSON* parsed = cJSON_Parse(json);
        free(json);
        if (!parsed) continue;

        cJSON* type = cJSON_GetObjectItem(parsed, "type");
        cJSON* source = cJSON_GetObjectItem(parsed, "source");
        bool matched = false;
        if (type && type->valuestring && strcmp(type->valuestring, "GOOSE") == 0 && source) {
            cJSON* goCbRef = cJSON_GetObjectItem(source, "goCbRef");
            if (goCbRef && goCbRef->valuestring && strcmp(goCbRef->valuestring, EXPECTED_GOCB_REF) == 0) {
                matched = true;
            }
        }
        cJSON_Delete(parsed);
        if (matched) return true;
    }
    return false;
}

static DeviceManagerHandle fixtureDeviceManager;
static SimServer fixtureSimA;
static SimServer fixtureSimB;

void
setUp(void) {
    DeviceManagerConfig config;
    DeviceManagerConfig_defaults(&config);
    config.wsPortRangeStart = DM_WS_PORT_RANGE_START;
    config.wsPortRangeEnd = DM_WS_PORT_RANGE_END;
    fixtureDeviceManager = DeviceManager_create(&config, NULL);

    fixtureSimA = SimServer_create();
    SimServer_setFilestoreBasepath(fixtureSimA, "fixtures/served_files/");
    SimServer_start(fixtureSimA, TEST_PORT_A);

    fixtureSimB = SimServer_create();
    SimServer_setFilestoreBasepath(fixtureSimB, "fixtures/served_files/");
    SimServer_start(fixtureSimB, TEST_PORT_B);

    Thread_sleep(200);
}

void
tearDown(void) {
    if (fixtureDeviceManager) {
        DeviceManager_destroy(fixtureDeviceManager);
        fixtureDeviceManager = NULL;
    }
    if (fixtureSimA) {
        SimServer_stop(fixtureSimA);
        SimServer_destroy(fixtureSimA);
        fixtureSimA = NULL;
    }
    if (fixtureSimB) {
        SimServer_stop(fixtureSimB);
        SimServer_destroy(fixtureSimB);
        fixtureSimB = NULL;
    }
}

void
test_twoConcurrentDevices_independentPortsAndReporting_thenStopAndPortReuse(void) {
    StartTask taskA;
    memset(&taskA, 0, sizeof(taskA));
    taskA.deviceManager = fixtureDeviceManager;
    taskA.host = "127.0.0.1";
    taskA.mmsPort = TEST_PORT_A;

    StartTask taskB;
    memset(&taskB, 0, sizeof(taskB));
    taskB.deviceManager = fixtureDeviceManager;
    taskB.host = "127.0.0.1";
    taskB.mmsPort = TEST_PORT_B;

    uint64_t wallStart = Hal_getTimeInMs();

    Thread threadA = Thread_create(startTaskRun, &taskA, false);
    Thread threadB = Thread_create(startTaskRun, &taskB, false);
    TEST_ASSERT_NOT_NULL(threadA);
    TEST_ASSERT_NOT_NULL(threadB);
    Thread_start(threadA);
    Thread_start(threadB);

    TEST_ASSERT_TRUE_MESSAGE(waitForTask(&taskA.done), "device A's StartReporting call never finished");
    TEST_ASSERT_TRUE_MESSAGE(waitForTask(&taskB.done), "device B's StartReporting call never finished");
    uint64_t wallElapsedMs = Hal_getTimeInMs() - wallStart;

    Thread_destroy(threadA);
    Thread_destroy(threadB);

    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_MANAGER_OK, taskA.result,
            "device A failed to start - if orchestrationDetail.stage==GOOSE_SUBSCRIBER_START, this test "
            "needs CAP_NET_RAW (run with sudo)");
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_MANAGER_OK, taskB.result,
            "device B failed to start - if orchestrationDetail.stage==GOOSE_SUBSCRIBER_START, this test "
            "needs CAP_NET_RAW (run with sudo)");

    TEST_ASSERT_TRUE_MESSAGE(taskA.deviceId != taskB.deviceId, "expected distinct deviceIds");
    TEST_ASSERT_TRUE_MESSAGE(taskA.wsPort != taskB.wsPort, "expected distinct websocket ports");

    /* Coarse, deliberately generous non-serialization bound: two real
     * bootstrap+MMS+GOOSE sequences run one-at-a-time would take roughly
     * twice as long as one; this just asserts the two ran close enough to
     * concurrently that they didn't fully serialize behind the registry's
     * lock (see this file's own header comment - the precise "lock isn't
     * held across the slow call" property is proven structurally by
     * tests/device_manager/test_device_manager_registry.c instead). */
    TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(15000, wallElapsedMs,
            "two concurrent StartReporting calls took suspiciously long - possible serialization");

    int wsA = connectAndUpgrade(taskA.wsPort);
    int wsB = connectAndUpgrade(taskB.wsPort);
    TEST_ASSERT_TRUE_MESSAGE(wsA >= 0, "websocket handshake to device A's own ipc_dispatcher failed");
    TEST_ASSERT_TRUE_MESSAGE(wsB >= 0, "websocket handshake to device B's own ipc_dispatcher failed");

    /* goose_subscriber bootstrap-suppresses the very first frame it ever
     * observes for a target (cached==NULL -> silently seed, never forward -
     * GOOSE's equivalent of MMS's GI suppression, see CLAUDE.md). This
     * simulator's GSE MinTime/MaxTime is 10ms/5000ms (sim_server.c), so
     * nothing guarantees a natural heartbeat lands between goose_subscriber
     * starting reception (just now, inside DeviceManager_startReporting) and
     * this flip - if none does, THIS flip's own resulting frame becomes the
     * "first frame ever" and would be silently suppressed, timing out the
     * assertion below with no further change ever occurring to reveal it.
     * integration_tests/goose_subscriber/'s own dedicated E2E test avoids
     * this by waiting for a VALID liveness status before flipping; this test
     * has no per-device liveness signal to wait on (only the outward-facing
     * ipc_dispatcher websocket), so it flips twice instead: the first flip is
     * a throwaway seed (whether or not it's actually the bootstrap frame),
     * the second is what's actually asserted on. */
    SimServer_setIndication(fixtureSimA, true);
    SimServer_setIndication(fixtureSimB, true);
    Thread_sleep(500);
    SimServer_setIndication(fixtureSimA, false);
    SimServer_setIndication(fixtureSimB, false);

    TEST_ASSERT_TRUE_MESSAGE(waitForGooseFrame(wsA), "expected a real GOOSE JSON message from device A");
    TEST_ASSERT_TRUE_MESSAGE(waitForGooseFrame(wsB), "expected a real GOOSE JSON message from device B");

    close(wsA);
    close(wsB);

    /* Stop A, confirm B is unaffected. */
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK, DeviceManager_stopReporting(fixtureDeviceManager, taskA.deviceId, NULL));

    int wsAAfterStop = connectAndUpgrade(taskA.wsPort);
    TEST_ASSERT_TRUE_MESSAGE(wsAAfterStop < 0, "device A's ipc_dispatcher port must be torn down after stop");

    SimServer_setIndication(fixtureSimB, false);
    int wsBStillUp = connectAndUpgrade(taskB.wsPort);
    TEST_ASSERT_TRUE_MESSAGE(wsBStillUp >= 0, "device B must still be running after only device A was stopped");
    close(wsBStillUp);

    /* Stop B, confirm the freed port is reusable by a subsequent start. */
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK, DeviceManager_stopReporting(fixtureDeviceManager, taskB.deviceId, NULL));

    uint64_t deviceIdC;
    uint16_t wsPortC;
    DeviceManagerErrorDetail detailC;
    DeviceManagerError startErrC = DeviceManager_startReporting(fixtureDeviceManager, "127.0.0.1", TEST_PORT_A,
            IED_NAME, TEST_INTERFACE, NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,&deviceIdC, &wsPortC, &detailC,
            NULL, NULL);
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK, startErrC);
    TEST_ASSERT_TRUE_MESSAGE(wsPortC == taskA.wsPort || wsPortC == taskB.wsPort,
            "expected one of the two freed ports to be reused rather than growing the range");

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK, DeviceManager_stopReporting(fixtureDeviceManager, deviceIdC, NULL));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_twoConcurrentDevices_independentPortsAndReporting_thenStopAndPortReuse);

    return UNITY_END();
}
