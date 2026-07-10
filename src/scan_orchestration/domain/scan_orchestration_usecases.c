#include <string.h>
#include "scan_orchestration/domain/scan_orchestration_usecases.h"

bool
ScanOrchestrationUseCases_isHostNew(const char* const* seenHosts, int seenCount, const char* host) {
    if (!host || !host[0]) return true;

    for (int i = 0; i < seenCount; i++) {
        if (seenHosts[i] && strcmp(seenHosts[i], host) == 0) return false;
    }
    return true;
}
