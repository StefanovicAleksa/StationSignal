#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main_discovery_prompt.h"

static void
printHostList(char** hosts, int count) {
    if (count == 0) {
        printf("  (no confirmed IEC 61850 devices yet - scan still running in background)\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        printf("  %d) %s\n", i + 1, hosts[i]);
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
MainDiscoveryPrompt_run(ScanOrchestrationHandle scanHandle, uint64_t scanId,
        IedDiscoveryHandle manualVerifyHandle, int mmsPort) {
    char* picked = NULL;
    char line[256];

    while (!picked) {
        char** hosts = NULL;
        int hostCount = 0;
        ScanOrchestration_snapshotDiscoveredHosts(scanHandle, scanId, &hosts, &hostCount);

        printf("\nConfirmed IEC 61850 devices:\n");
        printHostList(hosts, hostCount);
        printf("\nType a number to pick a device, or an IP address to add and verify a new candidate: ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            ScanOrchestration_freeDiscoveredHostsSnapshot(hosts, hostCount);
            break; /* stdin EOF - caller treats NULL as fatal */
        }
        chomp(line);
        if (!line[0]) {
            ScanOrchestration_freeDiscoveredHostsSnapshot(hosts, hostCount);
            continue;
        }

        if (isAllDigits(line)) {
            int idx = atoi(line) - 1;
            if (idx < 0 || idx >= hostCount) {
                printf("No such entry: %s\n", line);
            } else {
                picked = strdup(hosts[idx]);
            }
            ScanOrchestration_freeDiscoveredHostsSnapshot(hosts, hostCount);
            continue;
        }

        ScanOrchestration_freeDiscoveredHostsSnapshot(hosts, hostCount);

        IedDiscoveryError verifyErr;
        IedDiscoveryHostStatus status = IedDiscovery_verifyHost(manualVerifyHandle, line, mmsPort, &verifyErr);
        if (status == IED_DISCOVERY_HOST_CONFIRMED) {
            printf("%s confirmed.\n", line);
            picked = strdup(line); /* returned directly - never scanned, so never folded into the scan's own seen-set */
        } else {
            printf("%s: %s\n", line, hostStatusReason(status));
        }
    }

    return picked;
}
