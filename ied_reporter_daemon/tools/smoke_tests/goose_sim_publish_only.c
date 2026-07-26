#include <stdio.h>
#include "stdbool_compat.h"
#include "sim_types.h"
#include "hal_thread.h"

/*
 * Standalone probe (not part of the build/test system, mirrors
 * goose_loopback_smoke_test.c's throwaway-linkage-probe convention): starts
 * the real ied_simulator "Reporter1" IED (sim_server.c, the SAME code path
 * integration_tests/goose_subscriber's E2E test uses) with NOTHING else
 * running - no goose_subscriber, no receiver, nothing competing for the
 * interface. Its only job is to sit there and let IedServer's integrated
 * GOOSE publisher do whatever it does, so a parallel tcpdump capture can
 * show, independently of our own code, whether ANY GOOSE frames ever hit the
 * wire on "lo" - isolating "does the simulator publish at all" from "does
 * our subscriber receive it".
 *
 * REQUIRES CAP_NET_RAW (same as every other GOOSE test here) - run with sudo.
 *
 * Driven by ../../test_goose_sim_publishing.sh at the repo root.
 */

int
main(void) {
    SimServer sim = SimServer_create();

    printf("[SIM PUBLISH ONLY] Starting SimServer (MMS on 10205, GOOSE on 'lo')...\n");
    SimServer_start(sim, 10205);

    printf("[SIM PUBLISH ONLY] Sleeping 3s (birth/heartbeat GOOSE should appear on tcpdump by now)...\n");
    Thread_sleep(3000);

    printf("[SIM PUBLISH ONLY] Flipping GGIO1.Ind1.stVal (should trigger an on-change GOOSE)...\n");
    SimServer_setIndication(sim, true);

    printf("[SIM PUBLISH ONLY] Sleeping 10s more (periodic retransmissions should appear)...\n");
    Thread_sleep(10000);

    printf("[SIM PUBLISH ONLY] Stopping.\n");
    SimServer_stop(sim);
    SimServer_destroy(sim);

    return 0;
}
