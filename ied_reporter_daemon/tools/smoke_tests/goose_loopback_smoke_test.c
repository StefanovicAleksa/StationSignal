#include <stdio.h>
#include <stdlib.h>
#include "stdbool_compat.h"
#include "goose_publisher.h"
#include "goose_receiver.h"
#include "goose_subscriber.h"
#include "hal_thread.h"
#include "linked_list.h"
#include "mms_value.h"

/*
 * Standalone spike (not part of the build/test system) answering one question
 * before goose_subscriber's connection layer gets built: do raw GOOSE frames
 * actually round-trip over "lo" through libiec61850's specific hal_ethernet
 * implementation? Only vendored headers are available here (not libhal.a's
 * source), and lo's ARPHRD_LOOPBACK link-layer type is a known special case
 * for some raw-socket implementations - this proves or disproves it
 * empirically instead of assuming. VLAN tagging is disabled
 * (GoosePublisher_createEx's useVlanTag=false) to remove one variable at a
 * time, per the plan.
 *
 * Needs CAP_NET_RAW (raw AF_PACKET socket), same requirement as the daemon
 * itself per CLAUDE.md - run with sudo.
 *
 * Build + run:
 *   gcc -g -Wall -Isrc -idirafter third_party/include \
 *     tools/smoke_tests/goose_loopback_smoke_test.c \
 *     -o /tmp/goose_loopback_smoke_test \
 *     -Lthird_party/lib -liec61850 -lhal -lpthread
 *   sudo /tmp/goose_loopback_smoke_test
 */

#define TEST_INTERFACE "lo"
#define TEST_GOCB_REF "smokeTest/LLN0$GO$gcbSmoke"
#define TEST_APPID 0x1000

static volatile int messageReceived = 0;

static void
gooseListener(GooseSubscriber subscriber, void* parameter) {
    (void) parameter;

    printf("[SMOKE TEST] GOOSE message received on listener callback.\n");
    printf("[SMOKE TEST]   isValid=%d stNum=%u sqNum=%u appId=%d\n",
            GooseSubscriber_isValid(subscriber),
            GooseSubscriber_getStNum(subscriber),
            GooseSubscriber_getSqNum(subscriber),
            GooseSubscriber_getAppId(subscriber));

    MmsValue* values = GooseSubscriber_getDataSetValues(subscriber);
    if (values && MmsValue_getType(values) == MMS_ARRAY && MmsValue_getArraySize(values) > 0) {
        MmsValue* first = MmsValue_getElement(values, 0);
        if (first && MmsValue_getType(first) == MMS_BOOLEAN) {
            printf("[SMOKE TEST]   entry[0] (boolean) = %d\n", MmsValue_getBoolean(first));
        }
    }

    messageReceived = 1;
}

int
main(void) {
    CommParameters commParams = { 0 };
    commParams.vlanPriority = 4;
    commParams.vlanId = 0;
    commParams.appId = TEST_APPID;
    commParams.dstAddress[0] = 0x01;
    commParams.dstAddress[1] = 0x0c;
    commParams.dstAddress[2] = 0xcd;
    commParams.dstAddress[3] = 0x01;
    commParams.dstAddress[4] = 0x00;
    commParams.dstAddress[5] = 0x01;

    GoosePublisher publisher = GoosePublisher_createEx(&commParams, TEST_INTERFACE, false);
    if (!publisher) {
        fprintf(stderr, "[ERROR] GoosePublisher_createEx failed on interface '%s'\n", TEST_INTERFACE);
        return EXIT_FAILURE;
    }

    GoosePublisher_setGoCbRef(publisher, TEST_GOCB_REF);
    GoosePublisher_setDataSetRef(publisher, "smokeTest/LLN0$ds1");
    GoosePublisher_setConfRev(publisher, 1);
    GoosePublisher_setTimeAllowedToLive(publisher, 2000);

    GooseReceiver receiver = GooseReceiver_create();
    GooseReceiver_setInterfaceId(receiver, TEST_INTERFACE);

    GooseSubscriber subscriber = GooseSubscriber_create((char*) TEST_GOCB_REF, NULL);
    GooseSubscriber_setAppId(subscriber, TEST_APPID);
    GooseSubscriber_setListener(subscriber, gooseListener, NULL);
    GooseReceiver_addSubscriber(receiver, subscriber); /* must precede start() */

    GooseReceiver_start(receiver);

    if (!GooseReceiver_isRunning(receiver)) {
        fprintf(stderr, "[ERROR] GooseReceiver failed to start on interface '%s' - likely missing "
                "CAP_NET_RAW (run with sudo).\n", TEST_INTERFACE);
        GooseReceiver_destroy(receiver);
        GoosePublisher_destroy(publisher);
        return EXIT_FAILURE;
    }

    /* Give the receiver thread a moment to fully bind before publishing. */
    Thread_sleep(200);

    LinkedList dataSet = LinkedList_create();
    MmsValue* entry = MmsValue_newBoolean(true);
    LinkedList_add(dataSet, entry);

    printf("[SMOKE TEST] Publishing GOOSE message on interface '%s'...\n", TEST_INTERFACE);
    GoosePublisher_increaseStNum(publisher);
    int publishResult = GoosePublisher_publish(publisher, dataSet);
    if (publishResult != 0) {
        fprintf(stderr, "[ERROR] GoosePublisher_publish returned %d\n", publishResult);
    }

    /* Poll for up to 2 seconds for the listener to fire. */
    for (int i = 0; i < 20 && !messageReceived; i++) {
        Thread_sleep(100);
    }

    /* LinkedList_destroy is NOT shallow - it frees every element's data too
     * (per linked_list.h), which would double-free `entry` alongside the
     * explicit MmsValue_delete below. LinkedList_destroyStatic is the actual
     * shallow variant (list structure only, data left alone). */
    LinkedList_destroyStatic(dataSet);
    MmsValue_delete(entry);
    GooseReceiver_stop(receiver);
    GooseReceiver_destroy(receiver); /* cascades: destroys subscriber too */
    GoosePublisher_destroy(publisher);

    if (!messageReceived) {
        fprintf(stderr, "[SMOKE TEST] FAILED: no GOOSE message received over '%s' within timeout.\n",
                TEST_INTERFACE);
        return EXIT_FAILURE;
    }

    printf("[SMOKE TEST] PASSED: GOOSE frame round-tripped successfully over '%s'.\n", TEST_INTERFACE);
    return EXIT_SUCCESS;
}
