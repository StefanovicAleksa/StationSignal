#include <stdlib.h>
#include <string.h>
#include "features/goose_subscriber/domain/goose_subscriber_usecases.h"
#include "features/goose_subscriber/utils/goose_subscriber_utils.h"

#define GOOSE_SUBSCRIBER_MIN_LIVENESS_POLL_MS 50
#define GOOSE_SUBSCRIBER_DEFAULT_LIVENESS_POLL_MS 1000
#define GOOSE_SUBSCRIBER_LIVENESS_POLL_TAL_DIVISOR 4

static void
freeEntriesUpTo(GooseSubscriberEntry* entries, int builtCount) {
    for (int i = 0; i < builtCount; i++) {
        if (entries[i].value) MmsValue_delete(entries[i].value);
    }
    free(entries);
}

static GooseSubscriberEntry*
buildEntries(const MmsValue* dataSetValues, int entryCount) {
    if (entryCount <= 0) return NULL;

    GooseSubscriberEntry* entries = calloc((size_t) entryCount, sizeof(GooseSubscriberEntry));
    if (!entries) return NULL;

    for (int i = 0; i < entryCount; i++) {
        if (dataSetValues) {
            MmsValue* element = MmsValue_getElement((MmsValue*) dataSetValues, i);
            entries[i].value = element ? MmsValue_clone(element) : NULL;
        }
    }

    return entries;
}

GooseSubscriberRecord*
GooseSubscriberUseCases_buildRecord(
        const char* goCbRef, const char* goId, const char* dataSet,
        uint32_t stNum, uint32_t sqNum, uint32_t confRev,
        bool test, bool needsCommission,
        uint32_t timeAllowedToLiveMs, uint64_t timestampMs,
        bool hasVlan, uint16_t vlanId, uint8_t vlanPrio, int32_t appId,
        const uint8_t srcMac[6], const uint8_t dstMac[6],
        const MmsValue* dataSetValues, int entryCount) {
    GooseSubscriberRecord* record = calloc(1, sizeof(GooseSubscriberRecord));
    if (!record) return NULL;

    record->goCbRef = GooseSubscriberUtils_safeStringDup(goCbRef);
    record->goId = GooseSubscriberUtils_safeStringDup(goId);
    record->dataSet = GooseSubscriberUtils_safeStringDup(dataSet);

    record->stNum = stNum;
    record->sqNum = sqNum;
    record->confRev = confRev;
    record->test = test;
    record->needsCommission = needsCommission;
    record->timeAllowedToLiveMs = timeAllowedToLiveMs;
    record->timestampMs = timestampMs;

    record->hasVlan = hasVlan;
    record->vlanId = hasVlan ? vlanId : 0;
    record->vlanPrio = hasVlan ? vlanPrio : 0;
    record->appId = appId;

    if (srcMac) memcpy(record->srcMac, srcMac, 6);
    if (dstMac) memcpy(record->dstMac, dstMac, 6);

    record->entries = buildEntries(dataSetValues, entryCount);
    record->entryCount = record->entries ? entryCount : 0;

    return record;
}

void
GooseSubscriberUseCases_freeRecord(GooseSubscriberRecord* record) {
    if (!record) return;

    freeEntriesUpTo(record->entries, record->entryCount);
    free(record->goCbRef);
    free(record->goId);
    free(record->dataSet);
    free(record);
}

bool
GooseSubscriberUseCases_detectStatusTransition(bool wasValid, bool isValid, GooseSubscriberStatus* outStatus) {
    if (wasValid == isValid) return false;

    if (outStatus) {
        /* Caller (the liveness thread, which holds the live GooseSubscriber)
         * refines STALE vs INVALID_STATE via GooseSubscriber_getParseError()
         * when isValid goes false - this pure function can't reach that. */
        *outStatus = isValid ? GOOSE_SUBSCRIBER_STATUS_VALID : GOOSE_SUBSCRIBER_STATUS_STALE;
    }
    return true;
}

uint32_t
GooseSubscriberUseCases_computeLivenessPollIntervalMs(uint32_t configuredMs, int32_t minTalMs) {
    if (configuredMs > 0) return configuredMs;

    if (minTalMs <= 0) return GOOSE_SUBSCRIBER_DEFAULT_LIVENESS_POLL_MS;

    uint32_t derived = (uint32_t) minTalMs / GOOSE_SUBSCRIBER_LIVENESS_POLL_TAL_DIVISOR;
    return derived < GOOSE_SUBSCRIBER_MIN_LIVENESS_POLL_MS ? GOOSE_SUBSCRIBER_MIN_LIVENESS_POLL_MS : derived;
}
