#ifndef MAIN_DISCOVERY_PROMPT_H_
#define MAIN_DISCOVERY_PROMPT_H_

#include "features/ied_discovery/service/ied_discovery_api.h"
#include "scan_orchestration/service/scan_orchestration_api.h"

/*
 * Thin terminal-prompt adapter, deliberately kept out of ied_discovery's/
 * scan_orchestration's own service layers (see those features' own API doc
 * comments) so the interaction medium can be replaced later without touching
 * either feature itself. main.c is the only caller.
 *
 * Blocking. scanHandle/scanId identify an ALREADY-STARTED continuous
 * background scan (main.c owns its lifecycle - started via
 * ScanOrchestration_startScan before this call, stopped by main.c after this
 * call returns) - this function never starts or stops it, only reads its
 * live, growing result list. manualVerifyHandle is a SEPARATE, plain
 * IedDiscoveryHandle used only for the manually-typed-IP path, unchanged
 * from before this scan became continuous/backgrounded.
 *
 * Loops reading stdin lines, re-snapshotting scanId's current host list
 * (ScanOrchestration_snapshotDiscoveredHosts) fresh before every reprint -
 * the scan keeps running concurrently in the background the entire time this
 * loop is blocked on stdin, so newly discovered devices appear in the list
 * between prompts without this function polling on any timer of its own:
 *   - an all-digit line picks that 1-based numbered entry from the
 *     just-snapshotted list;
 *   - anything else is treated as a candidate IP, verified via
 *     IedDiscovery_verifyHost(manualVerifyHandle, ...) (same two-stage check
 *     the scan itself uses) - on success it's returned as the pick
 *     immediately (never folded into the scan's own seen-set, since it never
 *     participated in scanning at all), on failure the specific reason is
 *     printed and the loop continues.
 *
 * Returns a heap char* (caller frees) with the picked host, or NULL if
 * stdin hit EOF before a pick was made (caller should treat that as fatal).
 */
char*
MainDiscoveryPrompt_run(ScanOrchestrationHandle scanHandle, uint64_t scanId,
        IedDiscoveryHandle manualVerifyHandle, int mmsPort);

#endif /* MAIN_DISCOVERY_PROMPT_H_ */
