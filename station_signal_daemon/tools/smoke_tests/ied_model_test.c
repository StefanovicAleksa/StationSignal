#include <stdio.h>
#include <stdlib.h>
#include "features/ied_model/service/ied_model_api.h"

/* Verification harness for the ied_model feature: loads real reference SCL files
 * (not hand-crafted test fixtures) across all three AccessMode tiers and prints
 * resolved target counts + references, to prove the DataTypeTemplates recursion,
 * FCDA/DataSet resolution, ReportControl/GSEControl attachment, and the
 * missing-<Communication> degrade-gracefully path all work end to end. */

static void
printTargets(const char* label, LinkedList list) {
    printf("    %s: %d\n", label, LinkedList_size(list));
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        printf("      - %s\n", (char*) LinkedList_getData(element));
        element = LinkedList_getNext(element);
    }
}

static void
runCase(const char* path, const char* iedName) {
    printf("=== %s (IED: %s) ===\n", path, iedName);

    static const char* modeNames[] = { "REPORT_ONLY", "READ_ONLY", "READ_AND_WRITE" };
    static const AccessMode modes[] = {
        IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_ACCESS_READ_ONLY, IED_MODEL_ACCESS_READ_AND_WRITE
    };

    for (int i = 0; i < 3; i++) {
        IedModelLoadError err;
        IedModelHandle handle = IedModel_loadFromFile(path, iedName, modes[i], IED_MODEL_LN_CATEGORY_ALL, &err);
        if (!handle) {
            printf("  [%s] load failed, error=%d\n", modeNames[i], err);
            continue;
        }

        printf("  [%s]\n", modeNames[i]);
        LinkedList goose = IedModel_getGooseSubscriptionTargets(handle);
        LinkedList report = IedModel_getReportSubscriptionTargets(handle);
        LinkedList read = IedModel_getReadTargets(handle);
        LinkedList control = IedModel_getControlTargets(handle);

        printTargets("GOOSE", goose);
        printTargets("Report", report);
        printTargets("Read", read);
        printTargets("Control", control);

        LinkedList_destroyDeep(goose, free);
        LinkedList_destroyDeep(report, free);
        LinkedList_destroyDeep(read, free);
        LinkedList_destroyDeep(control, free);

        IedModel_release(handle);
    }

    printf("\n");
}

int
main(void) {
    /* No <Communication> section - exercises the missing-addressing path. */
    runCase("/home/aleksa/code/station_signal/libiec61850/tools/model_generator/complexModel.icd", "ied1");

    /* Has <Communication> - exercises full GSE addressing resolution. */
    runCase("/home/aleksa/code/station_signal/libiec61850/tools/model_generator/sampleModel_with_dataset.icd", "SampleIED");

    runCase("/home/aleksa/code/station_signal/libiec61850/tools/model_generator/inverter_with_report.icd", "ied1");

    return 0;
}
