#include <stdlib.h>
#include "features/scl_bootstrap/data/scl_bootstrap_tcp_probe.h"
#include "hal_socket.h"
#include "hal_time.h"
#include "hal_thread.h"

/* Polling granularity for checking in-flight async connects - short enough to
 * keep the whole scan responsive to per-probe timeouts without busy-spinning. */
#define POLL_INTERVAL_MS 10

typedef struct {
    Socket socket;
    int hostIndex;      /* index into hostList/result this slot is probing, or -1 if empty */
    uint64_t startTimeMs;
} ProbeSlot;

static const char*
hostAt(LinkedList hostList, int index) {
    LinkedList element = LinkedList_getNext(hostList);
    int i = 0;
    while (element) {
        if (i == index) return (const char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);
        i++;
    }
    return NULL;
}

bool*
SclBootstrapTcpProbe_scan(LinkedList hostList, int port, uint32_t timeoutMs, int maxConcurrent) {
    if (!hostList) return NULL;

    int total = LinkedList_size(hostList);
    if (total == 0) return NULL;
    if (maxConcurrent < 1) maxConcurrent = 1;
    if (maxConcurrent > total) maxConcurrent = total;

    bool* reachable = calloc((size_t) total, sizeof(bool));
    if (!reachable) return NULL;

    ProbeSlot* slots = calloc((size_t) maxConcurrent, sizeof(ProbeSlot));
    if (!slots) {
        free(reachable);
        return NULL;
    }
    for (int i = 0; i < maxConcurrent; i++) slots[i].hostIndex = -1;

    int nextToStart = 0;
    int remaining = total;

    while (remaining > 0) {
        /* Fill any empty slots from the remaining queue. */
        for (int i = 0; i < maxConcurrent && nextToStart < total; i++) {
            if (slots[i].hostIndex != -1) continue;

            const char* host = hostAt(hostList, nextToStart);
            Socket sock = TcpSocket_create();
            if (!sock) {
                /* Out of resources - count this candidate as unreachable and move on. */
                reachable[nextToStart] = false;
                nextToStart++;
                remaining--;
                continue;
            }

            if (!Socket_connectAsync(sock, host, port)) {
                Socket_destroy(sock);
                reachable[nextToStart] = false;
                nextToStart++;
                remaining--;
                continue;
            }

            slots[i].socket = sock;
            slots[i].hostIndex = nextToStart;
            slots[i].startTimeMs = Hal_getTimeInMs();
            nextToStart++;
        }

        bool anyResolvedThisRound = false;

        for (int i = 0; i < maxConcurrent; i++) {
            if (slots[i].hostIndex == -1) continue;

            SocketState state = Socket_checkAsyncConnectState(slots[i].socket);
            uint64_t elapsed = Hal_getTimeInMs() - slots[i].startTimeMs;

            if (state == SOCKET_STATE_CONNECTED) {
                reachable[slots[i].hostIndex] = true;
                Socket_destroy(slots[i].socket);
                slots[i].hostIndex = -1;
                remaining--;
                anyResolvedThisRound = true;
            } else if (state == SOCKET_STATE_FAILED || elapsed >= timeoutMs) {
                reachable[slots[i].hostIndex] = false;
                Socket_destroy(slots[i].socket);
                slots[i].hostIndex = -1;
                remaining--;
                anyResolvedThisRound = true;
            }
        }

        if (!anyResolvedThisRound && remaining > 0) {
            Thread_sleep(POLL_INTERVAL_MS);
        }
    }

    free(slots);
    return reachable;
}
