# `ied_model_online_loader`

Source: `src/features/ied_model_online_loader/`

## 1. Overview

`ied_model_online_loader` builds a complete `IedModelHandle` by walking a live
IED's MMS ACSI directory/model-discovery services — `GetLogicalDeviceList`,
`GetLogicalDeviceDirectory`, `GetLogicalNodeDirectory`, `GetDataDirectory[ByFC]`,
`GetDataSetDirectory`, `GetRCBValues`, `GetGoCBValues` — instead of parsing an
SCL file. It exists for real, connectable IEC 61850 devices (confirmed against
a real OMICRON IED Scout "Simulate IED" instance) that associate and browse
fine over MMS but never serve an SCL file (`.icd`/`.cid`/`.scd`/`.ssd`/`.sed`)
through file services at all — `scl_bootstrap` cannot succeed against them no
matter how the file directory is browsed, and without this feature such a
device is entirely unreportable.

This is **the one narrow, deliberate exception to CLAUDE.md's "No
over-the-wire tree discovery" Hard Rule**. It is never a silent, automatic
substitute for SCL parsing: it is engaged only via
`Orchestration_runFromOnlineDiscovery`, called explicitly by
`device_manager`'s bootstrap policy as a one-shot retry, and only after
`Orchestration_run`/`scl_bootstrap` has already failed with exactly
`SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND`. It is never invoked automatically
inside `Orchestration_run` itself. The result is an `IedModelHandle`
indistinguishable to every other feature from one built by
`IedModel_loadFromFile` — `mms_report_client`/`goose_subscriber` require zero
changes to consume it, because this feature ends by calling the same
`IedModel_wrapDynamicModel` entry point the SCL loader uses.

Public boundary: `src/features/ied_model_online_loader/service/ied_model_online_loader_api.h`.

## 2. Public API surface

```c
IedModelHandle
IedModelOnlineLoader_build(const char* host, int port, const char* iedName, AccessMode mode,
        const char* acseAuthPassword, const IedModelOnlineLoaderConfig* config,
        IedModelOnlineLoaderError* outError);
```

One entry point, one function. Contract notes:

- **Owns its own one-shot `IedConnection` end-to-end** — create, optional ACSE
  password auth, connect, discover, close+destroy — the same "owns its own MMS
  session" shape as `SclBootstrap_scanAndFetch`/`MmsReportClient_create`.
  Never takes a caller-supplied connection object. This is deliberate:
  `orchestration`'s own documented invariant is zero direct third-party
  includes, so the connection lifecycle has to live inside this feature, not
  in the caller.
- **Blocking, synchronous.** Returns only once the full model is built (or the
  attempt has failed) — no background thread, no async continuation.
- `iedName` only *labels* the constructed model — there is no SCL `<IED>` list
  to auto-detect from over a live connection. `NULL`/empty defaults to
  `"OnlineDiscovered"` inside `IedModel_wrapDynamicModel`.
- `mode` is the same `AccessMode` (`REPORT_ONLY`/`READ_ONLY`/`READ_AND_WRITE`)
  every other `ied_model` consumer uses, but this loader **only builds FC=ST/MX
  structure**. `IedModel_getReadTargets`/`_getControlTargets` against a model
  built here will see an incomplete/empty tree — an accepted v1 scope limit,
  since this entry point only ever drives report/GOOSE consumption.
- `acseAuthPassword` NULL means unauthenticated — same convention as
  `scl_bootstrap`/`mms_report_client`'s own config fields.
- `config->requestTimeoutMs` (0 = leave `IedConnection`'s own default
  untouched) is the only per-call tuning knob; this loader issues many more
  sequential MMS requests than a single SCL file fetch would, so a caller
  talking to a slow device may want to raise it.
- Returns `NULL` and sets `*outError` on failure. **Caller owns the returned
  handle exactly like `IedModel_loadFromFile`'s** — release via
  `IedModel_release`.

`IedModelOnlineLoaderError` (from `domain/ied_model_online_loader_types.h`):

| Value | Meaning |
|---|---|
| `IED_MODEL_ONLINE_LOADER_OK` | success |
| `IED_MODEL_ONLINE_LOADER_ERR_INVALID_ARGUMENT` | bad `host`/`port`, or `NULL` connection passed internally |
| `IED_MODEL_ONLINE_LOADER_ERR_CONNECT_FAILED` | `IedConnection_connect` itself failed |
| `IED_MODEL_ONLINE_LOADER_ERR_NO_LOGICAL_DEVICES` | `GetLogicalDeviceList` succeeded but returned zero LDs — a genuinely empty/misbehaving server, not this loader's fault to paper over |
| `IED_MODEL_ONLINE_LOADER_ERR_OUT_OF_MEMORY` | allocation failure (`IedConnection_createEx`, `IedModel_create`, or the final `IedModel_wrapDynamicModel`) |

## 3. Per-file breakdown

### `service/ied_model_online_loader_api.h`
Public boundary — the doc comment above `IedModelOnlineLoader_build` is the
canonical statement of this feature's role as the Hard Rule exception. Other
code (`orchestration`) must only include this header, never reach into
`domain`/`data` directly.

### `service/ied_model_online_loader_api.c`
Thin orchestrator, ~20 lines. `IedModelOnlineLoader_build`:
1. Calls `IedModelOnlineLoaderConnection_connectAndBuild` (the data layer) to
   get a raw dynamic `IedModel*`.
2. On success, wraps it via `IedModel_wrapDynamicModel(model, iedName, mode)`
   — the exact same call `ied_model`'s SCL loader path uses, which is what
   makes the result indistinguishable from an SCL-parsed handle.
3. If wrapping fails, destroys the raw `IedModel*` (`IedModel_destroy`) and
   reports `IED_MODEL_ONLINE_LOADER_ERR_OUT_OF_MEMORY` — no leak on that path.

### `domain/ied_model_online_loader_types.h`
Domain vocabulary: `IedModelOnlineLoaderError` enum and
`IedModelOnlineLoaderConfig` (currently just `requestTimeoutMs`). No
third-party includes — mixes libiec61850's MMS-client vocabulary (reached only
through the one opaque `IedConnection` the caller hands in) with `ied_model`'s
own dynamic-model vocabulary (`IedModel`), since this feature's whole job is
bridging the two.

### `domain/ied_model_online_loader_usecases.h` / `.c`
Pure reference-format conversion logic — no third-party includes, no
`IedConnection`, no `IedModel`. One function:

```c
char* IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(const char* acsiRef);
```

This is the mirror image of `mms_report_client`'s
`MmsReportClientUseCases_buildWireMemberReferences` (which converts this
codebase's `"$"`-joined wire form *into* `IedConnection_createDataSet`'s
required dot/bracket form). This function needs the **opposite** direction:
`IedConnection_getDataSetDirectory` hands back member references already in
ACSI dot/bracket form (`"LDName/LNodeName.item(arrayIndex)component[FC]"` per
`iec61850_client.h`'s own doc comment), and the caller
(`ied_model_online_loader_connection.c`'s `resolveAndBuildDataset`) needs this
codebase's `"$"`-joined, LD-prefix-free `"LN$FC$DO[$SDO...]$DA"` form to hand
straight to `DataSetEntry_create`.

Conversion steps, in order, on a copy of the input string:
1. **Trailing `[FC]` must be the very last thing in the string.** Finds the
   last `[` and last `]` via `strrchr`; if either is missing, mismatched, or
   the `]` isn't the final character, returns `NULL`. The bracket contents
   become `fc`.
2. **First `.` after the `"LD/LN"` prefix marks the start of the DO/SDO/DA
   chain** — LD and LN names never themselves contain a `.` (only FCDA path
   segments after them can, via nested SDOs). `strchr(copy, '.')` finds it;
   missing `.` → `NULL`.
3. **Strip the LD prefix.** The pre-dot portion is `"LD/LN"` (ACSI domain
   names join LD+LN with `/`). `DataSetEntry_create`'s own `variableName`
   argument must be LD-prefix-free (this codebase's documented dynamic-model
   gotcha #1 — the LD is conveyed separately/implicitly via which
   `LogicalNode` the entry's parent dataset belongs to). Finds the `/` via
   `strchr`; missing `/` → `NULL` (malformed — a real ACSI reference always
   has `"LD/LN"`).
4. **Walk the DO/SDO/DA chain**, splitting on `.` via `strtok`. Each token is
   passed through `stripArrayIndexAnnotation`, which drops any `"(...)"`
   array-index annotation (e.g. `"item(1)component"` → `"itemcomponent"`) —
   deliberately dropped, not preserved, since this codebase doesn't model
   array indices anywhere else (see `ied_model`'s own deferred `DAI/@ix`
   limitation) and preserving it would create a reference shape nothing
   downstream can consume. Cleaned tokens are joined with `$`.
5. **Assemble the final string**: `"<LN>$<FC>$<joined DO/SDO/DA chain>"`.

Returns `NULL` (not a best-effort partial string) on any malformed input or
allocation failure; a non-`NULL` result must be freed by the caller.

Example (from the unit tests): `"VR4C1C01A1LD0/SP16GGIO5.Ind.stVal[ST]"` →
`"SP16GGIO5$ST$Ind$stVal"`.

Note the LD-prefixed, public-facing `"LD/LN$FC$DO$DA"` reference form used
elsewhere (reference labeling, `ipc_dispatcher`'s quality pairing) is
reconstructed separately by `IedModel_getDataSetMemberReferences`, which
prepends the entry's own `logicalDeviceName` — never produced directly by this
function.

### `data/ied_model_online_loader_connection.h`
Declares the two data-layer entry points and documents the split between
them:

```c
IedModel* IedModelOnlineLoaderConnection_build(IedConnection conn, const char* iedName,
        const IedModelOnlineLoaderConfig* config, IedModelOnlineLoaderError* outError);

IedModel* IedModelOnlineLoaderConnection_connectAndBuild(const char* host, int port,
        const char* iedName, const char* acseAuthPassword,
        const IedModelOnlineLoaderConfig* config, IedModelOnlineLoaderError* outError);
```

`_build` takes an already-connected `IedConnection` and only ever reads from
it (never calls connect/destroy) — the data-layer counterpart to
`ied_model_scl_loader.c`'s mxml-tree-driven `IedModelSclLoader_load`, sourcing
the same shape of dynamic-model construction calls (`IedModel_create`,
`LogicalDevice_create`, ..., `ReportControlBlock_create`,
`GSEControlBlock_create`) from live MMS requests instead of a parsed SCL file.

`_connectAndBuild` owns the entire one-shot connection lifecycle around it.

### `data/ied_model_online_loader_connection.c` (the real work, 648 lines)

**Debug logging.** `appendDebugLog` appends timestamped lines to
`/tmp/station_signal_debug_model_build.log` (`IED_MODEL_ONLINE_LOADER_DEBUG_LOG_PATH`).
This is a temporary diagnostic aid added after real-hardware testing against a
device with no SCL file service showed Gap-4 decomposition never succeeding
for any dataset member, with no way to tell whether the live MMS
model-discovery walk itself was silently failing. It logs every ACSI-call
failure site inside the tree-building functions, plus a per-DO
built-child-count summary. Same append-per-call helper shape as
`mms_report_client`/`ipc_dispatcher`'s own debug logs — duplicated per this
codebase's cross-feature convention, not shared.

**String helpers**: `joinWithSeparator(a, sep, b)`, `buildLnRef(ldName,
lnName)` (→ `"LD/LN"`), `buildRcbDotRef(lnRef, buffered, rcbName)` (→
`"LD/LN.BR.name"` or `"LD/LN.RP.name"`), `buildAppIdHex(appId)` (→ 4-hex-digit
string).

**Dataset resolution** (shared by RCB and GoCB construction):
- `bareDatasetName(qualifiedRef)` — returns a borrowed pointer to the text
  after the last `$` in a qualified dataset reference. Both
  `ReportControlBlock_create`'s `dataSetName` and `GSEControlBlock_create`'s
  `dataSet` parameter want the **bare local name** (confirmed against
  `ied_model_scl_loader.c`'s own SCL-driven construction, and empirically the
  hard way: passing the fully-qualified `"LD/LN$ds1"` form produces a
  double-qualified, unresolvable reference once
  `IedModelUseCases_getReportSubscriptionTargets`/`_getGooseSubscriptionTargets`
  re-prepend their own `lnRef$` on top of it).
- `datasetAlreadyBuilt(builtDatasets, datasetRef)` — linear scan of an owned
  `char*` list.
- `resolveAndBuildDataset(conn, ln, datasetRef, builtDatasets)` — resolves one
  RCB/GoCB's non-empty dataset reference into a real `DataSet`/`DataSetEntry`
  chain via `IedConnection_getDataSetDirectory` (the wire equivalent of
  `IedModel_getDataSetMemberReferences`'s SCL-derived output). `ln` must be
  the RCB/GoCB's own parent LN — dataset and control block are always
  co-located under the same LN in this codebase's convention. De-dupes via
  `builtDatasets` (an owned `char*` list, scoped across the *whole* build, not
  just one LN — mirrors `mms_report_client`'s per-connect-cycle
  `dynamicDatasetCache`) since `DataSet_create` has no "already exists"
  tolerance. On failure to resolve, logs a `stderr` warning and returns
  (control block(s) referencing it will report/publish nothing — not a hard
  build failure). For each resolved member, calls
  `IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef` and skips
  (warns to stderr) any member that fails to convert.

**FC=ST/MX data object / data attribute tree.** Deliberately restricted to
FC=ST/MX only — the same "reportable" FC pair `IedModelUseCases_getReadTargets`/
`_getReportableAttributeReferencesForLogicalNode` already treat as the whole
story for reporting/GOOSE/dynamic-dataset purposes. This tree exists solely to
back `IedModel_getReportableAttributeReferencesForLogicalNode`'s
dynamic-dataset fallback for a discovered Dyn RCB/GoCB with no configured
dataset — dataset members themselves come from `getDataSetDirectory` directly
as flat strings (not from this tree), and RCB/GoCB configuration comes from
`getRCBValues`/`getGoCBValues` directly (also not from this tree). A
consequence: `IedModel_getReadTargets`/`_getControlTargets` (FC=CO and
generic-read paths) see an incomplete/empty tree against a model built by this
loader.

- `mapMmsTypeToDataAttributeType(MmsType)` — coarse, deliberately approximate
  mapping (`MMS_BOOLEAN`→`IEC61850_BOOLEAN`, `MMS_INTEGER`→`IEC61850_INT32`,
  `MMS_UNSIGNED`→`IEC61850_INT32U`, `MMS_FLOAT`→`IEC61850_FLOAT32`,
  `MMS_BIT_STRING`→`IEC61850_GENERIC_BITSTRING`, `MMS_OCTET_STRING`→
  `IEC61850_OCTET_STRING_64`, `MMS_VISIBLE_STRING`→`IEC61850_VISIBLE_STRING_255`,
  `MMS_UTC_TIME`→`IEC61850_TIMESTAMP`, default→`IEC61850_UNKNOWN_TYPE`).
  `MmsVariableSpecification` only carries the wire-level MMS basic type, which
  cannot distinguish CDC-semantic types sharing an MMS encoding (e.g.
  `IEC61850_QUALITY` vs. a generic 13-bit bitstring; `IEC61850_CODEDENUM` vs.
  a plain small bitstring) — guessing from naming convention alone would
  violate CLAUDE.md's "don't guess IEC 61850 semantics" rule. `MMS_UTC_TIME`
  is the one unambiguous exception. Callers skip creating a `DataAttribute`
  entirely for `IEC61850_UNKNOWN_TYPE` rather than guess a placeholder type.
- `buildDataAttributeTreeFromSpec(spec, parentNode, fc)` — builds a
  DataAttribute/DataObject subtree directly from one
  `MmsVariableSpecification`'s recursive component layout, bypassing
  `IedConnection_getDataDirectoryByFC` entirely. This is the fallback
  `buildFcContainer` engages when that call finds zero children.
  Root-caused against real hardware (confirmed by reading libiec61850's own
  client-side ACSI directory implementation, not just the vendored `.a`):
  `getDataDirectoryByFC` finds a DO's children by string-matching
  separately-named leaf entries in the domain's flat variable-name list, but
  real servers commonly expose a structured DO (e.g. an SPS's `stVal`/`q`/`t`)
  as **one** opaque `MMS_STRUCTURE`-typed named variable — exactly matching
  what a report's own wire value looks like — so the domain's flat name list
  never contains the `"$"`-suffixed leaf entries `getDataDirectoryByFC`'s
  matching depends on. One `getVariableSpecification` call on the DO returns
  the full recursive layout regardless of domain naming, needing no further
  wire round trips per leaf.
- `buildFcContainer(conn, nodeRef, fc, parentNode)` — one node's FC=`fc`
  children. Tries `IedConnection_getDataDirectoryByFC` first; if it returns a
  non-empty list, delegates to `buildFcContainerChildren`. If empty (or
  missing), falls back to `getVariableSpecification` +
  `buildDataAttributeTreeFromSpec`. If *both* fail, logs a `FAIL` line to the
  debug log with both error codes and leaves the node empty.
- `buildFcContainerChildren(conn, nodeRef, fc, parentNode, children)` —
  recurses per child: each child is either itself a container (its own
  `getDataDirectoryByFC` call returns non-empty — built as a `DataObject`,
  recursed into) or a genuine leaf (empty — type resolved via
  `getVariableSpecification`, built as a `DataAttribute`, or skipped +
  logged if the MMS type doesn't map). This "container = DataObject
  regardless of whether it's really an SDO or a CONSTRUCTED DA" choice is
  safe because every existing tree-walking accessor in
  `ied_model_usecases.c` already treats any non-`DataAttribute` node as
  "recurse unconditionally, filter once a leaf is reached" — it never itself
  distinguishes SDO from CONSTRUCTED DA containers.
- `buildLogicalNodeDataModel(conn, lnRef, ln)` — every FC=ST/MX-reportable DO
  under one LN. Gets the DO name list once via
  `getLogicalNodeDirectory(ACSI_CLASS_DATA_OBJECT)`, then for each DO name
  calls `buildFcContainer` twice (once for `IEC61850_FC_ST`, once for
  `IEC61850_FC_MX`) into the *same* `DataObject` node — a DO name appearing
  in both FC walks is only ever created once. Logs a per-DO
  `builtChildCount` summary line (flagging `<-- EMPTY, will forward as raw
  structure` when zero). If `getLogicalNodeDirectory` itself fails, logs a
  `FAIL` line and the whole LN's tree stays empty.

**Control blocks:**
- `buildReportControlBlocks(conn, lnRef, ln, acsiClass, buffered, builtDatasets)`
  — enumerates RCB names via `getLogicalNodeDirectory` with `ACSI_CLASS_BRCB`
  or `ACSI_CLASS_URCB`. For each, builds the dot-form RCB reference, calls
  `IedConnection_getRCBValues` to read the live `dataSetReference` and
  `confRev` (warns to stderr and creates with no dataset/confRev info on
  failure), then calls `ReportControlBlock_create` with `rptId=NULL` (library
  default: object reference) and `trgOps`/`options`/`bufTm`/`intgPd` left at
  0 — this daemon's `mms_report_client` never writes these live either way. A
  `NULL` dataset (the discovered "Dyn" case) is left as `NULL` unchanged —
  `mms_report_client`'s existing `getOrCreateDynamicDataset` handles it with
  no special-casing needed here. If a dataset *is* present, calls
  `resolveAndBuildDataset`.
- `buildGooseControlBlocks(conn, lnRef, ln, builtDatasets)` — enumerates GoCB
  names via `ACSI_CLASS_GoCB`. For each, calls `IedConnection_getGoCBValues`
  to read `datSet`, `confRev`, and `getDstAddress` (the non-deprecated
  by-value `PhyComAddress` accessor — the per-field `_appid`/`_vid`/
  `_priority`/`_addr` getters are deprecated per the vendored header).
  **`DstAddress` is an optional GoCB attribute** per IEC 61850-7-2, with no
  documented sentinel distinguishing "server never set this" from
  "explicitly all-zero" — this loader treats an all-zero `appId`+`vlanId`+MAC
  as "not populated" (`hasAddress = false`), an explicitly flagged,
  unverified-against-real-hardware heuristic. When not populated, the
  resulting `GooseSubscriptionTarget` degrades to unfiltered-by-address
  reception (`goose_subscriber` already handles `hasAddress==false` with zero
  special-casing), identical to today's "SCL with no `<GSE><Address>`" case.
  Builds the `GSEControlBlock` via `GSEControlBlock_create` with the hex
  `appId` string (or `""`), then conditionally calls
  `GSEControlBlock_addPhyComAddress` only if `hasAddress`. Also calls
  `resolveAndBuildDataset` if a dataset is present.

**Entry points:**
- `IedModelOnlineLoaderConnection_build(conn, iedName, config, outError)` —
  the real walk. Validates `conn` non-`NULL`. Calls
  `IedConnection_getLogicalDeviceList`; zero LDs →
  `IED_MODEL_ONLINE_LOADER_ERR_NO_LOGICAL_DEVICES`. Creates the dynamic model
  via **`IedModel_create("")`** — deliberately an *empty* name, not
  `iedName`. `iedName` only labels the resulting `IedModelHandle` later, via
  `IedModel_wrapDynamicModel` — independent of this call. The empty name is
  load-bearing: `LogicalDevice_create` implicitly **prepends** its parent
  `IedModel`'s own name to the name it's given, to form the LD's real wire
  name. This was confirmed empirically the hard way — feeding
  `getLogicalDeviceList`'s already-fully-qualified names (e.g.
  `"Reporter1LD1"`) through `LogicalDevice_create(name, model)` with a model
  named `"Reporter1"` produced a corrupted double-prefixed wire name,
  `"Reporter1Reporter1LD1"` (a third dynamic-model construction gotcha
  alongside the two already documented for `DataSet`/`DataSetEntry`). Using
  `""` as the model name sidesteps ever needing to know/derive the server's
  true IED name at all.

  Then, for each LD name (unmodified, straight off the wire):
  `LogicalDevice_create(ldName, model)`, then `getLogicalDeviceDirectory` for
  the LN name list (stderr warning + LD left with no LNs on failure), then
  for each LN: `LogicalNode_create`, build `lnRef`, and call
  `buildLogicalNodeDataModel`, `buildReportControlBlocks` (twice — BRCB then
  URCB), and `buildGooseControlBlocks` in sequence. `builtDatasets` is a
  single `LinkedList` shared across the entire walk, freed at the end
  alongside `ldNames`.

- `IedModelOnlineLoaderConnection_connectAndBuild(host, port, iedName,
  acseAuthPassword, config, outError)` — validates `host`/`port`, creates the
  connection via `IedConnection_createEx(NULL, true)`, applies
  `requestTimeoutMs` if configured, calls
  `IedModelOnlineLoaderAuth_configurePasswordAuth`, then
  `IedConnection_connect`. On connect failure: destroys the connection,
  returns `IED_MODEL_ONLINE_LOADER_ERR_CONNECT_FAILED`. On success: delegates
  to `IedModelOnlineLoaderConnection_build`, then **always**
  `IedConnection_close` + `IedConnection_destroy` before returning — one-shot,
  nothing to keep the association open for once discovery is done or has
  failed, unlike `mms_report_client`'s long-lived connection reused across
  reconnects for the daemon's whole lifetime.

### `data/ied_model_online_loader_auth.h` / `.c`
`IedModelOnlineLoaderAuth_configurePasswordAuth(conn, password)` — no-op if
`password` is `NULL`. Reaches through `IedConnection_getMmsConnection` →
`MmsConnection_getIsoConnectionParameters`, creates an
`AcseAuthenticationParameter` with `ACSE_AUTH_PASSWORD` mechanism, and attaches
it to the ISO connection params before connect. Byte-for-byte the same
approach as `scl_bootstrap`'s `data/scl_bootstrap_auth.c` and
`mms_report_client`'s `data/mms_report_client_auth.c` — **deliberately
duplicated, not shared**: features never reach into each other's
data/domain layers, only service headers.

## 4. Threading & concurrency model

Fully synchronous and blocking. `IedModelOnlineLoader_build` runs entirely on
the calling thread — no worker thread, no callback registration, no async
continuation. It performs a large sequential burst of MMS request/response
round trips (one `IedConnection` from create through the whole ACSI walk to
close+destroy) and returns only when the walk is complete or has failed. There
is nothing to synchronize internally: only one thread ever touches the
`IedConnection` this feature owns, and it never outlives the call. The only
concurrency-relevant fact for callers is that this call can take
substantially longer than a single SCL file fetch (see §5) — callers on a
latency-sensitive path (e.g. `device_manager`'s registry lock) should be aware
it blocks for the whole walk.

## 5. Known limitations / deliberate scope boundaries

All v1 limitations named in CLAUDE.md, verified against the code:

- **Only builds FC=ST/MX structure.** `buildLogicalNodeDataModel` only ever
  calls `buildFcContainer` with `IEC61850_FC_ST` and `IEC61850_FC_MX`
  (`ied_model_online_loader_connection.c` lines ~371-372). Confirmed:
  `IedModel_getReadTargets`/`_getControlTargets` (FC=CO and generic-read
  paths) see an incomplete/empty tree against a model built here.
- **`DataAttributeType` is a coarse MMS wire-type mapping.**
  `mapMmsTypeToDataAttributeType` maps by raw `MmsType` only (8 cases + a
  catch-all `IEC61850_UNKNOWN_TYPE`) — cannot recover CDC semantics
  (`IEC61850_QUALITY` vs. generic bitstring, `IEC61850_CODEDENUM` vs. plain
  bitstring) since `MmsVariableSpecification` doesn't carry that information
  on the wire. Confirmed harmless for every current `ied_model` accessor
  (none read `DataAttributeType` back off a built node — only
  `ModelNode_getType()`/FC are consulted) — this only matters if a future
  feature starts relying on real CDC semantics from a model built by this
  loader.
- **GoCB addressing "populated" heuristic.** `buildGooseControlBlocks` treats
  an all-zero `appId`+`vlanId`+MAC as "not populated" — confirmed unverified
  against a real vendor device (the E2E test only proves the mechanism works
  against a compliant `ied_simulator` server that *does* populate it).
- **No dataset-count/`maxAttributes` cap handling.** `resolveAndBuildDataset`
  builds every member `getDataSetDirectory` returns with no chunking or
  count-limit logic — confirmed, no such check exists anywhere in this file.
- **Slower than one SCL transfer + local parse.** Confirmed structurally: the
  walk is O(LDs × LNs × (DOs × 2 FCs × recursive-child-calls + RCBs + GoCBs +
  datasets)) sequential MMS round trips, vs. one file transfer + local mxml
  parse for the SCL path. `IedModelOnlineLoaderConfig.requestTimeoutMs` exists
  specifically because of this.

Additional scope notes visible in the code but not spelled out as a numbered
CLAUDE.md limitation:
- The debug log (`appendDebugLog` → `/tmp/station_signal_debug_model_build.log`)
  is explicitly a **temporary diagnostic aid** pending further real-hardware
  confirmation, same pattern as `mms_report_client`'s own
  `station_signal_debug_entryid.log` — not a permanent logging feature.
- RCB `trgOps`/`options`/`bufTm`/`intgPd` are always created at 0 — this
  loader never attempts to discover or preserve a server's live trigger-option
  configuration.

## 6. Cross-feature dependencies

- **Calls into `ied_model`**: the sole integration point is
  `IedModel_wrapDynamicModel(model, iedName, mode)` in
  `service/ied_model_online_loader_api.c` — the exact same function
  `ied_model`'s SCL loader path uses to wrap an mxml-parsed model, which is
  what makes every `ied_model` accessor (`IedModel_getReportSubscriptionTargets`,
  `IedModel_getGooseSubscriptionTargets`, `IedModel_getDataSetMemberReferences`,
  etc.) behave identically regardless of origin.
- **Invoked by `orchestration`**: only through
  `Orchestration_runFromOnlineDiscovery(handle, host, mmsPort, iedName,
  interfaceId, accessMode, acseAuthPassword, outDetail)`
  (`src/orchestration/service/orchestration_api.h`). This joins the same
  shared continuation (`mms_report_client` start → `goose_subscriber` start)
  every other `Orchestration_run*` entry point uses — only stage 0-1 (model
  acquisition) differs from the SCL paths.
- **Relationship to `scl_bootstrap`**: strictly sequential, never concurrent
  and never automatic. `device_manager`'s bootstrap policy calls
  `Orchestration_run` first (which internally uses `scl_bootstrap`); only if
  that fails with exactly `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` does the
  policy retry once via `Orchestration_runFromOnlineDiscovery`. Any other
  `scl_bootstrap` failure (e.g. `SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED`)
  does **not** trigger this fallback — confirmed distinguished in the E2E
  test's own top comment, which notes this was verified empirically during
  the test's own development.
- **No relationship to `ied_discovery`/`scan_orchestration`** — this feature
  is not part of subnet scanning; it operates on one already-known
  `host:port`, same as `scl_bootstrap`/`mms_report_client`.
- **Downstream consumers unaffected**: `mms_report_client` and
  `goose_subscriber` require zero code changes to consume a handle built by
  this loader — they only ever see `IedModelHandle`.

## 7. Tests

**Unit** — `tests/ied_model_online_loader/test_ied_model_online_loader_usecases.c`
(wired into `tests/Makefile` as `test_ied_model_online_loader_usecases`,
building only `domain/ied_model_online_loader_usecases.c`, no third-party
deps). Covers `IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef`
exhaustively:
- DO-level member (`"...LD0/SP16GGIO5.Ind[ST]"` → `"SP16GGIO5$ST$Ind"`) —
  explicitly framed as a regression test for a real-hardware bug where the
  function used to leave the LD prefix in place, silently breaking
  `LogicalNode` lookup for every online-discovered dataset member and
  disabling Gap-4 decomposition entirely.
- Leaf member with a full DO.DA chain.
- Nested SDO chain (`PhV.phsA.cVal.mag.f`).
- Array-index annotation stripped (`Ind(1)component` → `Indcomponent`).
- Malformed inputs return `NULL`: missing `LD/LN` slash, missing `[FC]`,
  missing dot chain after the LD/LN prefix, `NULL` input.

**Integration (E2E)** —
`integration_tests/ied_model_online_loader/e2e_test_ied_model_online_loader.c`
(Makefile target `e2e_test_ied_model_online_loader`, no `sudo` — MMS/TCP over
loopback only). Runs a real `ied_simulator` "Reporter1" IED in-process with
its MMS file services pointed at an empty fixture directory
(`fixtures/no_scl_files/`), reproducing the exact scenario this feature exists
for. Three tests:
1. `test_fileServicesDisabled_scl_bootstrapReportsNoSclFileFound` — proves the
   precondition is real: `scl_bootstrap` genuinely returns
   `SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND` against this server (not
   assumed).
2. `test_onlineDiscovery_buildsReportTargets_matchingSimServerShape` — builds
   a model via `IedModelOnlineLoader_build` against the same server and
   checks `IedModel_getReportSubscriptionTargets` finds both `brcbMain`
   (buffered, under `LLN0`) and `urcbDyn` (unbuffered, under `GGIO1`), and
   that a server-side `NULL` dataset is faithfully reported as `NULL` rather
   than fabricated.
3. `test_onlineDiscovery_buildsGooseTargets_matchingSimServerShape` — checks
   `IedModel_getGooseSubscriptionTargets` finds `gcbInd` with its real
   `DstAddress` (`appId=0x1000`, `vlanId=10`, MAC `01-0c-cd-01-00-01`) decoded
   correctly (`hasAddress=true`), and that
   `IedModel_getDataSetMemberReferences` against its dataset resolves both
   members (`GGIO1$ST$Ind1$stVal`, `GGIO1$ST$Ind1$q`) via a real
   `GetDataSetDirectory` round trip through
   `IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef` — this is the one
   place in the E2E suite that proves dataset-member resolution end-to-end
   (RCBs in the simulator are all created with `NULL` datasets by design, so
   only the GoCB path exercises this).

No test in either suite exercises a real subnet-topology-dependent scan or a
real vendor device — the all-zero GoCB-address heuristic (§5) is explicitly
flagged as unverified beyond what this fixture can prove.
