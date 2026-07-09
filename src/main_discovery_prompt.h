#ifndef MAIN_DISCOVERY_PROMPT_H_
#define MAIN_DISCOVERY_PROMPT_H_

#include "features/ied_discovery/service/ied_discovery_api.h"

/*
 * Thin terminal-prompt adapter, deliberately kept out of ied_discovery's own
 * service layer (see that feature's API doc comment) so the interaction
 * medium can be replaced later without touching the feature itself. main.c
 * is the only caller.
 *
 * Blocking. Runs an initial IedDiscovery_scanSubnet on interfaceId/mmsPort
 * and prints the confirmed hosts as a numbered list (even if empty - keeps
 * prompting regardless, never exits just because the scan found nothing).
 * Then loops reading stdin lines:
 *   - an all-digit line picks that 1-based numbered entry;
 *   - anything else is treated as a candidate IP, verified via
 *     IedDiscovery_verifyHost (same two-stage check as the scan itself) -
 *     on success it's appended to the list and the list is reprinted, on
 *     failure the specific reason is printed and the loop continues.
 *
 * Returns a heap char* (caller frees) with the picked host, or NULL if
 * stdin hit EOF before a pick was made (caller should treat that as fatal).
 */
char*
MainDiscoveryPrompt_run(IedDiscoveryHandle handle, const char* interfaceId, int mmsPort);

#endif /* MAIN_DISCOVERY_PROMPT_H_ */
