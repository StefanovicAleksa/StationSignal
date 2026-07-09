#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main_discovery_prompt.h"

static const char*
hostAt(LinkedList list, int index) {
    LinkedList element = LinkedList_getNext(list);
    int i = 0;
    while (element) {
        if (i == index) return (const char*) LinkedList_getData(element);
        element = LinkedList_getNext(element);
        i++;
    }
    return NULL;
}

static void
printHostList(LinkedList hosts) {
    int count = LinkedList_size(hosts);
    if (count == 0) {
        printf("  (no confirmed IEC 61850 devices yet)\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        printf("  %d) %s\n", i + 1, hostAt(hosts, i));
    }
}

static bool
isAllDigits(const char* s) {
    if (!s || !s[0]) return false;
    for (const char* c = s; *c; c++) {
        if (*c < '0' || *c > '9') return false;
    }
    return true;
}

static void
chomp(char* line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

static const char*
hostStatusReason(IedDiscoveryHostStatus status) {
    switch (status) {
        case IED_DISCOVERY_HOST_CONFIRMED:
            return "confirmed";
        case IED_DISCOVERY_HOST_NOT_TCP_REACHABLE:
            return "not reachable on that port (no TCP connect)";
        case IED_DISCOVERY_HOST_NOT_MMS_DEVICE:
            return "reachable, but didn't respond as a real IEC 61850 MMS device";
        default:
            return "unknown";
    }
}

char*
MainDiscoveryPrompt_run(IedDiscoveryHandle handle, const char* interfaceId, int mmsPort) {
    IedDiscoveryError scanErr;
    printf("Scanning %s for IEC 61850 MMS devices on port %d...\n", interfaceId, mmsPort);

    LinkedList hosts = IedDiscovery_scanSubnet(handle, interfaceId, mmsPort, &scanErr);
    if (!hosts) {
        printf("Scan failed (error %d) - you can still type IPs manually below.\n", (int) scanErr);
        hosts = LinkedList_create();
    }

    char* picked = NULL;
    char line[256];

    while (!picked) {
        printf("\nConfirmed IEC 61850 devices:\n");
        printHostList(hosts);
        printf("\nType a number to pick a device, or an IP address to add and verify a new candidate: ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break; /* stdin EOF - caller treats NULL as fatal */
        }
        chomp(line);
        if (!line[0]) continue;

        if (isAllDigits(line)) {
            const char* chosen = hostAt(hosts, atoi(line) - 1);
            if (!chosen) {
                printf("No such entry: %s\n", line);
                continue;
            }
            picked = strdup(chosen);
            break;
        }

        IedDiscoveryError verifyErr;
        IedDiscoveryHostStatus status = IedDiscovery_verifyHost(handle, line, mmsPort, &verifyErr);
        if (status == IED_DISCOVERY_HOST_CONFIRMED) {
            LinkedList_add(hosts, strdup(line));
            printf("%s confirmed and added.\n", line);
        } else {
            printf("%s: %s\n", line, hostStatusReason(status));
        }
    }

    LinkedList_destroyDeep(hosts, free);
    return picked;
}
