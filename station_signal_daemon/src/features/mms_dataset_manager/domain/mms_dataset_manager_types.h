#ifndef MMS_DATASET_MANAGER_TYPES_H_
#define MMS_DATASET_MANAGER_TYPES_H_

#include "stdbool_compat.h"
#include "linked_list.h"
#include "iec61850_client.h"
#include "features/ied_model/service/ied_model_api.h"

/*
 * Domain vocabulary for this feature IS libiec61850's MMS-client dataset
 * vocabulary (IedConnection, ClientReportControlBlock, IedClientError) plus
 * ied_model's ReportControlBlockTarget - same convention as
 * mms_report_client's own domain layer (and ied_model's before it): this data
 * genuinely is the feature's domain, not swappable infrastructure. The pure
 * logic in mms_dataset_manager_usecases.h is deliberately free of all of it,
 * taking plain strings/arrays instead, so it stays unit-testable with no live
 * server.
 *
 * WHAT THIS FEATURE IS FOR
 * ------------------------
 * Everything about an IED's report DATASETS - discovering which ones already
 * exist (SCL-declared static ones, live-assigned dynamic ones, foreign ones
 * left by a commissioning tool), and filling whatever coverage gap is left
 * over with datasets of its own - lives here. It runs against an already-
 * loaded ied_model and an already-connected IedConnection, and it hands back
 * one resolved dataset per Report Control Block target.
 *
 * It deliberately knows NOTHING about enabling RCBs, report handlers, the
 * value-diff cache, EntryID resumption or reconnect supervision - those are
 * mms_report_client's job, which calls this feature once per connect cycle
 * and binds whatever dataset comes back to the RCB it asked about. The split
 * exists because those two responsibilities had grown into one 3300-line
 * file; every behavior on both sides of the line is unchanged by the split
 * itself.
 */

typedef enum {
    MMS_DATASET_MANAGER_OK = 0,
    MMS_DATASET_MANAGER_ERR_INVALID_ARGUMENT,
    MMS_DATASET_MANAGER_ERR_OUT_OF_MEMORY
} MmsDatasetManagerError;

/*
 * Which of the four resolution tiers actually produced a target's dataset -
 * see MmsDatasetManager_resolveForTarget's own doc comment
 * (mms_dataset_manager_api.h) for the full ordering and rationale. The caller
 * needs this because the two shape-reconciliation paths on ITS side differ by
 * tier: a live/adopted dataset's real member list was resolved over the wire
 * and may not match anything locally known, while a self-created one's
 * members are exactly the cluster this feature planned for it.
 */
typedef enum {
    MMS_DATASET_TIER_NONE = 0,   /* nothing resolved - see MmsDatasetResolution.wasNeeded */
    MMS_DATASET_TIER_SCL,        /* tier 1: SCL declared a static datSet for this RCB */
    MMS_DATASET_TIER_LIVE,       /* tier 2: the device already had one assigned to this RCB */
    MMS_DATASET_TIER_ADOPTED,    /* tier 3: an existing, unclaimed dataset under this RCB's own LD */
    MMS_DATASET_TIER_SELF_CREATED /* tier 4: created (or reused-by-name) by this feature */
} MmsDatasetTier;

/*
 * One target's resolved dataset. The caller owns every field and must release
 * it with MmsDatasetManager_destroyResolution, exactly once, however the
 * caller's own sequence ends.
 */
typedef struct {
    /* OWNED copy of the dataset reference to bind to this RCB, or NULL if no
     * tier resolved one. Deliberately an owned copy rather than the borrowed
     * pointer each tier hands back internally: tier 2's own borrowed pointer
     * aliases the ClientReportControlBlock's internal MmsValue buffer, which
     * the caller's own ClientReportControlBlock_setDataSetReference call would
     * then feed straight back into itself (MmsValue_setVisibleString frees and
     * reallocates that exact buffer when the new string is longer) - benign
     * today only because the strings are identical in length. Owning it
     * removes the hazard outright. */
    char* datasetReference;

    MmsDatasetTier tier;

    /* Static, never-freed string naming `tier` for logs ("SCL"/"live"/
     * "adopted"/"self-created"/"none") - the caller logs the resolution tier
     * per RCB unconditionally, so this saves it a switch of its own. */
    const char* tierName;

    /* OWNED LinkedList of owned "$"-joined, LD-prefixed member-reference
     * char* (mms_report_client's memberReferences[] convention), i.e. the real
     * decode shape every report against this RCB must be interpreted against.
     *
     * NULL means "nothing to reconcile" and is NOT the same as an empty list:
     *   - tier SCL: always NULL - the caller already resolved this shape
     *     locally at start time from the same immutable SCL datSet.
     *   - tier LIVE/ADOPTED: the dataset's real member list, resolved live.
     *   - tier SELF_CREATED: this target's own planned cluster members. NULL
     *     if no cluster was assigned to it at all; EMPTY if a cluster was
     *     assigned but holds no members (the caller stamps its fingerprint
     *     without rebuilding, rather than retrying the rebuild every enable).
     *   - tier NONE: NULL. */
    LinkedList memberReferences;

    /* Only meaningful when datasetReference is NULL, and the ONLY signal
     * separating the two genuinely different outcomes that share that case:
     *
     *   false => this target was NEVER NEEDED this cycle. Whole-device
     *            clustering ran out of clusters before it ran out of Dyn RCB
     *            slots, i.e. the device's reportable data is already fully
     *            covered by other targets. A benign, expected outcome on any
     *            device carrying redundant spare RCB instances (a real
     *            SIPROTEC has dozens) - the caller must leave such an RCB
     *            completely untouched and must NOT report it as a failure.
     *   true  => a dataset genuinely WAS needed here and could not be
     *            obtained (budget exhausted, createDataSet rejected, no
     *            wire-convertible members, or a malformed model with no LN
     *            reference). This RCB's share of the device's data will go
     *            unreported - a real problem.
     *
     * Before this distinction existed the two were byte-identical to the
     * caller, which buried the handful of real failures under a pile of
     * benign ones. Always written, including on every success path (where it
     * is false - nothing is outstanding once a dataset resolved). */
    bool wasNeeded;
} MmsDatasetResolution;

/*
 * One entry per Dyn RCB target that was assigned a dynamically-created
 * dataset within the current connect cycle - see
 * MmsDatasetManagerProvisioning_getOrCreateDataset's own doc comment. Built
 * fresh in MmsDatasetManager_beginCycle for every (re)connect and discarded at
 * MmsDatasetManager_endCycle; never carried across reconnects (the @-scoped
 * datasets it names don't survive a reconnect either).
 */
typedef struct {
    char* lnReference;  /* owned copy, matches ReportControlBlockTarget.objectReference -
                            every Dyn target gets its own uniquely-clustered dataset now,
                            keyed by its own objectReference, never shared with another
                            target's own assignment (field name kept for minimal diff
                            against this struct's original single-LN-dedup design). */
    char* datasetName;  /* owned copy, e.g. "@dyn_E13_6MD_PTOC1" (unbuffered) or
                            "E13_6MD/PTOC1$dyn" (buffered) */
    bool buffered;      /* part of the lookup key alongside lnReference - technically
                            redundant now that every key is already a unique
                            objectReference, kept for the naming-scheme distinction
                            MmsDatasetManagerNaming_buildDatasetName still needs
                            (buffered vs association-scoped). */
} MmsDatasetManagerCacheEntry;

/*
 * One whole-device cluster (a DO-atomic group of reportable leaves, possibly
 * spanning several different LNs - see
 * MmsDatasetManagerProvisioning_buildWholeDeviceClusterPlan's own doc comment
 * for how/when this gets populated and by which of its two strategies)
 * assigned to ONE Dyn RCB slot anywhere on the device - 1:1, never shared.
 * rcbReference matches that target's own objectReference (not lnReference -
 * every assigned target gets its own unique dataset, since a cluster's content
 * generally has nothing to do with the target's own parent LN anymore).
 */
typedef struct {
    char* rcbReference;      /* owned copy, == the assigned target's own objectReference */
    char** memberReferences; /* owned array of owned strings - this chunk's own DO-atomic subset */
    int memberCount;
} MmsDatasetManagerChunkAssignment;

/*
 * Everything one connect cycle needs: the existing per-target dedup cache,
 * plus TWO budget counters seeded from SCL's own <Services> declarations - one
 * per dataset pool a createDataSet attempt can actually draw from:
 *
 *   - remainingDynBudget: <DynDataSet max="N"/> (IedModel_getDynDataSetMax),
 *     UNCORRECTED - nothing this feature can see ahead of time pre-exists in
 *     this pool (association-specific datasets live inside another
 *     connection's own private association state, invisible to
 *     MmsDatasetManagerDiscovery_findExistingServerDatasets' per-LD directory
 *     query - see that function's own doc comment). Decremented only on a
 *     genuinely new successful association-specific createDataSet.
 *   - remainingConfBudget: <ConfDataSet max="N"/> (IedModel_getConfDataSetMax),
 *     CORRECTED for datasets already discovered on the server
 *     (MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget) at
 *     beginCycle - not just a blind copy of the declared max, which has no
 *     awareness of what's already consuming the device's real budget (leftover
 *     domain-scoped datasets from an earlier ungracefully-terminated run,
 *     other clients'/tools' own datasets, etc. - a real device run showed
 *     exactly this silently exhausting the real budget while this feature's own
 *     naive counter still believed most of it remained). This is the ONLY pool
 *     discovery can ever correct, since discovery can only ever see
 *     domain-scoped (Conf-class) datasets in the first place. Decremented on
 *     any genuinely new successful DOMAIN-SCOPED createDataSet - a buffered
 *     target's own primary attempt (always domain-scoped) and an unbuffered
 *     target's fallback attempt both draw from this same pool.
 *
 * Both model "how many MORE of our own new createDataSet attempts this
 * connect cycle may still make" in their own pool. -1 (SCL never declared a
 * cap) must never trigger either short-circuit - see
 * MmsDatasetManagerUseCases_isDynamicDatasetBudgetExhausted. Neither is ever
 * decremented on a cache hit or an adopted existing dataset (both are free,
 * regardless of pool). dynBudgetExhaustedLogged/confBudgetExhaustedLogged
 * edge-trigger their own "stopping" log so each fires once per cycle per
 * pool, not once per remaining target - without this, a device with many
 * RCBs past a budget wall would print one near-identical line per remaining
 * target instead of one.
 *
 * Built fresh in MmsDatasetManager_beginCycle for every (re)connect - server-side
 * state may have genuinely changed since the last connect, so discovery
 * re-runs fresh every time too. `active` distinguishes a live cycle from a
 * never-begun/already-ended one, so a stray resolve/end call is a safe no-op.
 */
typedef struct {
    bool active;
    LinkedList cache;            /* MmsDatasetManagerCacheEntry* list */
    int remainingDynBudget;
    bool dynBudgetExhaustedLogged;
    int remainingConfBudget;
    bool confBudgetExhaustedLogged;
    LinkedList chunkAssignments; /* MmsDatasetManagerChunkAssignment* list - empty (never NULL)
                                     only if the device has no Dyn RCB slots at all or no
                                     reportable data anywhere in the model. */
    LinkedList existingServerDatasets; /* owned char* list - every dataset already on the server
                                     across this client's own Dyn targets' LDs, discovered once
                                     per connect cycle. */
    LinkedList claimedDatasetNames; /* owned char* list - tracks which existingServerDatasets
                                     entries have already been tried/claimed this cycle, whether
                                     successfully adopted or found unusable. */
    bool associationSpecificCreateRejected; /* Latched the first time an association-specific
                                     ("@"-prefixed) createDataSet fails and the domain-scoped
                                     fallback then succeeds. Some real devices (confirmed:
                                     SIPROTEC 6MD) reject the association-specific FORM outright,
                                     every time, independent of quota; without this latch every
                                     unbuffered Dyn target spends one doomed ~100-member
                                     createDataSet round-trip before falling back, which on a real
                                     device was nine wasted large PDUs per connect cycle.
                                     Per-cycle only, never persisted, so a device that starts
                                     accepting them is retried on the next connect. */
} MmsDatasetManagerSession;

typedef struct sMmsDatasetManagerHandle* MmsDatasetManagerHandle;

/*
 * Internal representation. Defined here (rather than hidden behind an
 * additional internal-only header) because every file within this feature
 * needs field access, mirroring mms_report_client's struct
 * sMmsReportClientHandle and ied_model's struct sIedModelHandle convention:
 * opacity is enforced by which header is exposed
 * (service/mms_dataset_manager_api.h is the only public one), not by hiding
 * the struct.
 */
struct sMmsDatasetManagerHandle {
    IedModelHandle iedModel;  /* borrowed - caller retains ownership */

    /* BORROWED - this feature deliberately never opens an association of its
     * own. An association-specific ("@"-prefixed) dataset created on a
     * different connection would be invisible and unassignable to the RCB the
     * caller is enabling on ITS connection, and a second association against
     * an already-loaded device is exactly the kind of extra load this codebase
     * works to avoid. The caller owns this connection's whole lifecycle;
     * everything here must run while it is live (notably
     * MmsDatasetManager_cleanupOnStop, which must be called BEFORE
     * IedConnection_close). */
    IedConnection connection;

    /* BORROWED - the caller's own ReportControlBlockTarget* list, fixed for
     * the client's whole lifetime. Must outlive this handle. */
    LinkedList targets;

    /* Owned: char* list of domain/VMD-scoped ("$"-joined, no "@" prefix)
     * dynamic dataset names self-created for RCBs whose SCL declared no
     * datSet - see MmsDatasetManagerNaming_buildDatasetName's own doc comment
     * for why a buffered RCB can't use the ordinary association-scoped
     * ("@"-prefixed) self-created dataset an unbuffered RCB gets: an
     * association-scoped dataset is destroyed the instant the connection
     * closes, which is incompatible with a buffered RCB's whole purpose of
     * surviving one. Unlike that "@"-scoped scheme, these names are NOT
     * auto-cleaned by the server on disconnect, so
     * MmsDatasetManager_cleanupOnStop explicitly IedConnection_deleteDataSet's
     * every name here before the caller closes the connection, to avoid
     * leaking into the device's own total dataset-count budget across repeated
     * start/stop cycles. Deliberately persists across reconnects (unlike
     * MmsDatasetManagerSession's own per-connect-cycle cache) precisely so
     * cleanupOnStop can find every name ever created for this handle's whole
     * lifetime, not just the current cycle's - deduped on insert, since naming
     * is deterministic per target and a reconnect re-derives the same name
     * rather than a new one. */
    LinkedList domainScopedDynamicDatasetNames;

    /* The current connect cycle, or an all-zero/inactive struct between
     * cycles - see MmsDatasetManagerSession's own doc comment. Only ever
     * touched from the caller's own supervisor thread, which drives one cycle
     * at a time strictly sequentially (begin -> resolve* -> end), so this
     * needs no lock of its own. */
    MmsDatasetManagerSession session;
};

#endif /* MMS_DATASET_MANAGER_TYPES_H_ */
