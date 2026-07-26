#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdbool_compat.h"
#include "iec61850_client.h"
#include "iec61850_common.h"
#include "mms_client_connection.h"
#include "iso_connection_parameters.h"
#include "linked_list.h"

/*
 * Standalone diagnostic probe (not part of the build/test system): connects
 * to one live IED and dumps its actual logical-device/logical-node/RCB
 * directory over real MMS GetServerDirectory/GetLogicalDeviceDirectory/
 * GetLogicalNodeDirectory ACSI services, so the output can be diffed
 * directly against what a local SCL file claims that IED's object model
 * looks like. Written to chase down a real "getRCBValues failed: error 22
 * (IED_ERROR_OBJECT_DOES_NOT_EXIST)" seen for every RCB when loading SCL
 * from a local file (Orchestration_runFromLocalFile) against a live ABB
 * REC650 exposed via IED Scout - the object-reference format mms_report_client
 * builds already matches libiec61850's own documented convention
 * ("<ldName>/<ln>.<RP|BR>.<name>", iec61850_client.h), so if the live device
 * genuinely doesn't have those objects, the loaded SCL is stale/mismatched
 * relative to what's actually running - this tool proves that empirically
 * instead of guessing.
 *
 * Read-only: never touches the model tree, never writes to the server, no
 * concern with this repo's "no over-the-wire tree discovery" Hard Rule
 * (that rule is about production runtime behavior at startup, not a
 * throwaway manual diagnostic - same class as goose_loopback_smoke_test.c).
 *
 * Build + run:
 *   gcc -g -Wall -Isrc -idirafter third_party/include \
 *     tools/smoke_tests/mms_directory_dump_smoke_test.c \
 *     -o /tmp/mms_dir_dump -Lthird_party/lib -liec61850 -lhal -lpthread
 *   /tmp/mms_dir_dump <host> <port> [ldName] [acseAuthPassword]
 *
 * ldName: dump only this logical device (matches SCD's IEDName+LDInst,
 * e.g. "VR4C1C01A1LD0"). Omit or pass "" to dump every LD on the server.
 */

static void
configurePasswordAuth(IedConnection conn, const char* password) {
    if (!conn || !password || !password[0]) return;

    MmsConnection mmsConn = IedConnection_getMmsConnection(conn);
    if (!mmsConn) return;

    IsoConnectionParameters isoParams = MmsConnection_getIsoConnectionParameters(mmsConn);
    if (!isoParams) return;

    AcseAuthenticationParameter authParam = AcseAuthenticationParameter_create();
    if (!authParam) return;

    AcseAuthenticationParameter_setAuthMechanism(authParam, ACSE_AUTH_PASSWORD);
    AcseAuthenticationParameter_setPassword(authParam, (char*) password);
    IsoConnectionParameters_setAcseAuthenticationParameter(isoParams, authParam);
}

static void
dumpRcbClass(IedConnection conn, const char* lnRef, ACSIClass acsiClass, const char* label) {
    IedClientError err = IED_ERROR_OK;
    LinkedList names = IedConnection_getLogicalNodeDirectory(conn, &err, lnRef, acsiClass);

    if (err != IED_ERROR_OK) {
        printf("      %s: <error %d>\n", label, (int) err);
        return;
    }

    if (!names || LinkedList_size(names) == 0) {
        printf("      %s: (none)\n", label);
    } else {
        LinkedList element = LinkedList_getNext(names);
        while (element) {
            printf("      %s: %s\n", label, (char*) LinkedList_getData(element));
            element = LinkedList_getNext(element);
        }
    }

    if (names) LinkedList_destroy(names);
}

static void
dumpLogicalDevice(IedConnection conn, const char* ldName) {
    printf("  LogicalDevice %s\n", ldName);

    IedClientError err = IED_ERROR_OK;
    LinkedList lnNames = IedConnection_getLogicalDeviceDirectory(conn, &err, ldName);

    if (err != IED_ERROR_OK) {
        printf("    <GetLogicalDeviceDirectory error %d>\n", (int) err);
        return;
    }

    if (!lnNames || LinkedList_size(lnNames) == 0) {
        printf("    (no logical nodes)\n");
    } else {
        LinkedList element = LinkedList_getNext(lnNames);
        while (element) {
            const char* lnName = (char*) LinkedList_getData(element);
            printf("    LN %s\n", lnName);

            char lnRef[256];
            snprintf(lnRef, sizeof(lnRef), "%s/%s", ldName, lnName);

            dumpRcbClass(conn, lnRef, ACSI_CLASS_BRCB, "BRCB");
            dumpRcbClass(conn, lnRef, ACSI_CLASS_URCB, "URCB");

            element = LinkedList_getNext(element);
        }
    }

    if (lnNames) LinkedList_destroy(lnNames);
}

int
main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <host> <port> [ldName] [acseAuthPassword]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* host = argv[1];
    int port = atoi(argv[2]);
    const char* ldNameFilter = (argc > 3 && argv[3][0]) ? argv[3] : NULL;
    const char* acseAuthPassword = (argc > 4 && argv[4][0]) ? argv[4] : NULL;

    IedConnection conn = IedConnection_create();
    if (!conn) {
        fprintf(stderr, "IedConnection_create failed\n");
        return EXIT_FAILURE;
    }

    configurePasswordAuth(conn, acseAuthPassword);

    IedClientError err = IED_ERROR_OK;
    IedConnection_connect(conn, &err, host, port);

    if (err != IED_ERROR_OK) {
        fprintf(stderr, "IedConnection_connect(%s:%d) failed: error %d\n", host, port, (int) err);
        IedConnection_destroy(conn);
        return EXIT_FAILURE;
    }

    printf("Connected to %s:%d\n", host, port);

    if (ldNameFilter) {
        dumpLogicalDevice(conn, ldNameFilter);
    } else {
        LinkedList ldNames = IedConnection_getLogicalDeviceList(conn, &err);

        if (err != IED_ERROR_OK) {
            fprintf(stderr, "GetServerDirectory (logical devices) failed: error %d\n", (int) err);
        } else if (!ldNames || LinkedList_size(ldNames) == 0) {
            printf("(server reports no logical devices)\n");
        } else {
            LinkedList element = LinkedList_getNext(ldNames);
            while (element) {
                dumpLogicalDevice(conn, (char*) LinkedList_getData(element));
                element = LinkedList_getNext(element);
            }
        }

        if (ldNames) LinkedList_destroy(ldNames);
    }

    IedConnection_close(conn);
    IedConnection_destroy(conn);
    return EXIT_SUCCESS;
}
