#ifndef MMS_DATASET_MANAGER_PROVISIONING_H_
#define MMS_DATASET_MANAGER_PROVISIONING_H_

#include "features/mms_dataset_manager/domain/mms_dataset_manager_types.h"

/*
 * INTERNAL to this feature - not part of the public boundary
 * (service/mms_dataset_manager_api.h is). The CREATE side: planning which
 * part of the device each spare Dyn RCB slot should cover, actually creating
 * those datasets against the real budget, and cleaning up the ones this
 * client itself left behind. Only reached once every reuse tier in
 * mms_dataset_manager_discovery.h has come up empty.
 */

/*
 * Computes, once per connect cycle (called from MmsDatasetManager_beginCycle),
 * a device-WIDE dataset plan covering every "Dyn" (SCL datSet="Dyn",
 * target->datasetReference == NULL) RCB slot on this IED - not just the ones
 * parented on an LN that itself has a Dyn RCB. A Dyn RCB's own parent LN does
 * NOT restrict what a dataset assigned to it can report on (dataset members
 * are independently addressed per-element, not tied to any one LN -
 * IedModel_getReportableAttributeReferencesForWholeDevice's own doc comment
 * has the full citation trail), so every Dyn RCB slot anywhere on the device
 * is treated as a fungible reporting channel that can be pointed at ANY part
 * of the device's reportable data - covering LDs/LNs that have zero RCBs of
 * their own (e.g. a real SIPROTEC device where ~28 of ~30 LDs have no RCB at
 * all, while one LD's LLN0 alone has dozens of otherwise-redundant spare RCB
 * instances that used to all just duplicate the same tiny LLN0-only dataset).
 *
 * Two clustering strategies, chosen by whether SCL declared a real
 * maxAttributes cap - preferring ConfDataSet's own declared cap
 * (IedModel_getConfDataSetMaxAttributes) over DynDataSet's
 * (IedModel_getDynDataSetMaxAttributes), falling back to Dyn's if Conf isn't
 * declared: every buffered target's self-created dataset is always
 * domain-scoped (Conf-class) regardless, and on a device that structurally
 * rejects association-specific creation (confirmed against a real SIPROTEC
 * 6MD - see CHANGELOG.md), so does every unbuffered target's dataset once it
 * falls back. Sizing the ONE shared plan against the larger, more-often-
 * relevant cap directly shrinks the total number of clusters needed for
 * full-device coverage. The one accepted tradeoff: a target whose real
 * association-specific attempt would have succeeded at Dyn's smaller cap, but
 * is now sized above it, spends one extra doomed attempt before correctly
 * falling back to Conf - not a correctness bug (the fallback already handles
 * arbitrary Dyn failures), just a narrow case traded for fewer datasets
 * overall on devices where Dyn creation never succeeds anyway.
 *   - KNOWN (>0): the whole device's leaf list is packed via
 *     MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice - a single
 *     resulting cluster may legitimately span several different (small) LNs'
 *     worth of leaves when they fit together, maximizing how much of the
 *     device fits within a tight total dataset-count budget.
 *   - UNKNOWN (-1 or 0 on both caps, e.g. no <Services> at all, or a
 *     dynamically-built online-discovered model):
 *     MmsDatasetManagerUseCases_groupReferencesByLn packs one dataset per LN
 *     instead, unbounded size - the same per-LN granularity this codebase
 *     always used before whole-device clustering existed, since combining
 *     multiple LNs into one dataset without a known size bound risks an
 *     oversized, doomed createDataSet call.
 *
 * Clusters are assigned to Dyn slots in simple model-declaration order
 * (handle->targets' own existing order - no attempt to prioritize which part
 * of the device matters more, there's no signal to rank by since this
 * codebase never polls). Whichever list (clusters or slots) runs out first
 * determines the shortfall, logged plainly either way - never a silent drop:
 * more clusters than slots means part of the device goes unreported this
 * cycle; more slots than clusters means some RCB instances simply have
 * nothing left to assign (the device is already fully covered).
 *
 * Recomputed fresh every connect cycle rather than cached on the handle: a
 * pure function of already-static data (handle->iedModel, handle->targets,
 * both fixed for the handle's whole lifetime), so recomputation is cheap
 * (string work over the model's own size, no wire calls) and idempotent.
 *
 * Returns a LinkedList of owned MmsDatasetManagerChunkAssignment* (never
 * NULL, empty if there is nothing to plan). Caller owns:
 * LinkedList_destroyDeep(list, MmsDatasetManagerProvisioning_destroyChunkAssignment).
 */
LinkedList
MmsDatasetManagerProvisioning_buildWholeDeviceClusterPlan(MmsDatasetManagerHandle handle);

/* Looks up the cluster this cycle's plan assigned to one RCB, or NULL if that
 * slot got none (the device's reportable data was already fully covered by
 * other slots). Borrowed - owned by the session's own plan. */
MmsDatasetManagerChunkAssignment*
MmsDatasetManagerProvisioning_findChunkAssignment(MmsDatasetManagerSession* session, const char* rcbReference);

/*
 * Tier 4 of the resolution order, for an RCB whose SCL declared no datSet
 * (datasetReference == NULL, datSet="Dyn" in SCL terms): resolves (creating
 * if needed) the dataset buildWholeDeviceClusterPlan assigned this target
 * this cycle - a cluster of reportable leaves that may span several different
 * LNs across the WHOLE device, not just this target's own parent LN. Always
 * keyed/named by target->objectReference - no LN-wide dedup/sharing (every
 * Dyn target gets its own uniquely-clustered dataset, since two different
 * targets' clusters generally have nothing in common).
 *
 * Returns the dataset name (borrowed - owned by the session's own cache,
 * valid for the rest of the current connect cycle) on success, or NULL if
 * this target received no cluster assignment this cycle, if this cycle's own
 * dataset-count budget is already exhausted, or if dataset creation itself
 * failed (cap exceeded, maxAttributes exceeded, etc.).
 *
 * *outWasNeeded distinguishes those NULL cases from each other, and is the
 * ONLY signal that does - see MmsDatasetResolution.wasNeeded's own doc
 * comment (mms_dataset_manager_types.h) for the full rule. Always written
 * before any return, including the success paths (where it is false).
 */
const char*
MmsDatasetManagerProvisioning_getOrCreateDataset(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        bool* outWasNeeded);

/*
 * Proactive orphan cleanup, called from MmsDatasetManager_endCycle:
 * reclaims budget from OUR OWN domain-scoped datasets sitting on the server
 * but not needed by any target this cycle - the ungraceful-restart gap
 * MmsDatasetManager_cleanupOnStop can't close on its own (a killed/crashed
 * daemon never reaches that code path at all - the real-world scenario that
 * motivated this whole discovery/reuse/cleanup pass), now closed proactively
 * on every successful connect instead of only on a graceful stop.
 *
 * STRICT, conservative safety bar: a discovered name is only ever deleted if
 * it exactly reconstructs via
 * MmsDatasetManagerNaming_buildDatasetName(target->objectReference, true) for
 * some real buffered Dyn target in handle->targets right now - never a name
 * that merely looks like it might be ours, and never anything that doesn't
 * hit this exact bar. A foreign dataset, regardless of origin, is never
 * deleted here - only ever adopted - deletion is destructive and must stay
 * limited to datasets this client can prove it created itself.
 *
 * "Not needed this cycle" means the name never got claimed (adopted, or
 * found-unusable-and-skipped - either way tracked in
 * session->claimedDatasetNames) and never ended up in session->cache for its
 * own target's create-or-reuse resolution - i.e. genuinely idle leftover
 * capacity, not something actively serving a report right now. Before
 * deleting, disables RptEna and clears DatSet on the matched target's RCB if
 * (and only if) its live DatSet still is this candidate - same fix as
 * cleanupOnStop's own graceful cleanup, needed here too since a crashed
 * daemon never reached that code path to do it itself, and a still-live
 * RptEna/DatSet blocks deletion regardless of which code path eventually
 * attempts it. Best-effort throughout: any failure just leaves the dataset
 * behind, logged, not fatal. *outLeakedCount (optional) is incremented once
 * per dataset the server refused to delete.
 */
void
MmsDatasetManagerProvisioning_cleanupOrphaned(MmsDatasetManagerHandle handle, int* outLeakedCount);

/*
 * The graceful-stop counterpart of cleanupOrphaned - see
 * MmsDatasetManager_cleanupOnStop (the public wrapper) for the full contract
 * and the hard requirement that this runs BEFORE IedConnection_close.
 */
void
MmsDatasetManagerProvisioning_cleanupOnStop(MmsDatasetManagerHandle handle);

/* LinkedListValueDeleteFunction-compatible destructors for the two owned
 * entry types the session's own lists hold. NULL-safe. */
void
MmsDatasetManagerProvisioning_destroyCacheEntry(void* entry);

void
MmsDatasetManagerProvisioning_destroyChunkAssignment(void* entry);

#endif /* MMS_DATASET_MANAGER_PROVISIONING_H_ */
