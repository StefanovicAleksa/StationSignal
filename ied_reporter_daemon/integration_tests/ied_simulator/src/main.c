#include <stdio.h>
#include <stdlib.h>
#include "sim_types.h"
#include "hal_thread.h"

/*
 * Standalone entry point for manual testing: runs the "Reporter1" simulated
 * IED (see sim_types.h for its model shape) and periodically flips
 * GGIO1.Ind1.stVal so a manually-attached MMS client (this daemon, or any
 * third-party IEC 61850 client tool) can observe reports in real time.
 * Not used by the automated E2E test, which links sim_server.c directly and
 * drives SimServer itself instead of spawning this process.
 *
 * Optional argv[2]: if given, requires ACSE password auth (SimServer_requireAuthentication)
 * with this password - lets an out-of-process test harness (e.g. ied_reporter_api's own
 * integration tests, which build and spawn this binary rather than linking sim_server.c
 * directly) exercise a password-protected device end to end.
 */

#define DEFAULT_PORT 10202
#define FLIP_INTERVAL_MS 5000

int
main(int argc, char** argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    SimServer sim = SimServer_create();
    if (argc > 2) SimServer_requireAuthentication(sim, argv[2]);
    SimServer_start(sim, port);

    printf("[ied_simulator] Reporter1 listening on port %d. Ctrl-C to stop.\n", port);

    /* Runs until killed (Ctrl-C) - the OS reclaims the listening socket and
     * threads on exit, so no signal-driven shutdown path is implemented for
     * this manual testing tool. */
    bool value = false;
    for (;;) {
        Thread_sleep(FLIP_INTERVAL_MS);
        value = !value;
        printf("[ied_simulator] flipping GGIO1.Ind1.stVal -> %s\n", value ? "true" : "false");
        SimServer_setIndication(sim, value);
    }
}
