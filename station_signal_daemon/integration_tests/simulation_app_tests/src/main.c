#include <stdio.h>
#include <stdlib.h>
#include "sim_types.h"
#include "hal_thread.h"

/*
 * Standalone entry point for running several distinguishable simulated IEDs
 * concurrently (see run_simulated_ieds.sh at the repo root), for manual
 * frontend/API scan+connect testing against a real network interface - not
 * used by any automated test. Adapted from
 * integration_tests/ied_simulator/src/main.c (see sim_types.h's own header
 * comment for the full rationale); that original stays single-instance/
 * loopback-only and untouched.
 *
 * Periodically flips GGIO1.Ind1.stVal so a manually-attached MMS/GOOSE client
 * (the daemon, or any third-party IEC 61850 client tool) observes live
 * reports.
 *
 * filestoreBasepath enables MMS file services (even though nothing SCL-like
 * lives there) purely so the daemon's scl_bootstrap sees a real (if empty)
 * file directory rather than file services being unsupported outright -
 * IedConnection_getFileDirectory against a server with no file service at
 * all fails the whole browse step (SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED,
 * a hard error device_manager's bootstrap policy does NOT retry), whereas an
 * empty-but-real directory fails only "no SCL file found among these entries"
 * (SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND), which IS retried automatically
 * via Orchestration_runFromOnlineDiscovery (walks the live MMS model
 * directly - exactly what this simulator, with no actual SCL file, needs).
 *
 * Usage: ied_sim_app <bindIp> <mmsPort> <iedName> <gooseInterfaceId> <filestoreBasepath> [password]
 *
 * Optional trailing [password]: if given, requires ACSE password auth
 * (SimServer_requireAuthentication) with this password - lets run_simulated_ieds.sh mark one
 * or more simulated devices as password-protected for manual frontend testing of the
 * AUTH_REQUIRED/password-prompt flow (see station_signal_api/docs/FRONTEND_API_GUIDE.md §5).
 */

#define FLIP_INTERVAL_MS 5000

int
main(int argc, char** argv) {
    if (argc != 6 && argc != 7) {
        fprintf(stderr,
                "usage: %s <bindIp> <mmsPort> <iedName> <gooseInterfaceId> <filestoreBasepath> [password]\n",
                argv[0]);
        return 1;
    }

    const char* bindIp = argv[1];
    int port = atoi(argv[2]);
    const char* iedName = argv[3];
    const char* gooseInterfaceId = argv[4];
    const char* filestoreBasepath = argv[5];
    const char* password = (argc == 7) ? argv[6] : NULL;

    SimServer sim = SimServer_create(iedName);
    SimServer_setFilestoreBasepath(sim, filestoreBasepath);
    if (password) SimServer_requireAuthentication(sim, password);
    SimServer_start(sim, bindIp, port, gooseInterfaceId);

    if (password) {
        printf("[ied_sim_app] %s requires ACSE password auth (password: %s)\n", iedName, password);
    }

    printf("[ied_sim_app] %s listening on %s:%d, GOOSE on %s. Ctrl-C to stop.\n", iedName, bindIp, port,
            gooseInterfaceId);

    /* Runs until killed (Ctrl-C) - the OS reclaims the listening socket and
     * threads on exit, so no signal-driven shutdown path is implemented for
     * this manual testing tool. */
    bool value = false;
    for (;;) {
        Thread_sleep(FLIP_INTERVAL_MS);
        value = !value;
        printf("[ied_sim_app] %s flipping GGIO1.Ind1.stVal -> %s\n", iedName, value ? "true" : "false");
        SimServer_setIndication(sim, value);
    }
}
