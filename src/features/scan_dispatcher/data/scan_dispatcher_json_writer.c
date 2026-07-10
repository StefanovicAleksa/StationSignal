#include "cJSON.h"
#include "features/scan_dispatcher/data/scan_dispatcher_json_writer.h"

char*
ScanDispatcherJsonWriter_write(const ScanDeviceFoundEvent* event) {
    if (!event) return NULL;

    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "schemaVersion", 1);
    cJSON_AddStringToObject(root, "type", "SCAN_RESULT");
    cJSON_AddNumberToObject(root, "scanId", (double) event->scanId);
    cJSON_AddStringToObject(root, "host", event->host ? event->host : "");
    cJSON_AddNumberToObject(root, "mmsPort", event->mmsPort);
    cJSON_AddNumberToObject(root, "discoveredAtMs", (double) event->discoveredAtMs);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json;
}
