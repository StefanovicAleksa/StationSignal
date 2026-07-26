# `ied_model`

Source: `src/features/ied_model/`

## 1. Overview

`ied_model` loads an IED's IEC 61850 data model from SCL (`.icd`/`.cid`/`.scd`) and exposes it
as an opaque, queryable handle. It is the one and only source of truth for "what does this IED's
model look like" in this daemon — `goose_subscriber` and `mms_report_client` both get their
subscription targets (which RCBs/GoCBs exist, what their datasets contain, what a reference
string looks like) exclusively through this feature's public API. Neither of them ever
re-parses SCL or re-derives a reference itself. This is a direct instance of the codebase-wide
"No over-the-wire tree discovery" Hard Rule: the model is built once, locally, from a file (or,
for the one narrow `ied_model_online_loader` exception, from a live device's own ACSI directory
services then wrapped into the same shape) — never rediscovered per-subscription.

The feature also gates capability by `AccessMode` (`IED_MODEL_ACCESS_REPORT_ONLY` <
`IED_MODEL_ACCESS_READ_ONLY` < `IED_MODEL_ACCESS_READ_AND_WRITE`, always a strict hierarchy).
The loader always builds the *complete* model regardless of mode — gating happens only at the
service-layer accessor, not at load time. `IedModel_getGooseSubscriptionTargets` /
`IedModel_getReportSubscriptionTargets` are available at every mode (reporting is the daemon's
baseline function); `IedModel_getReadTargets` needs `READ_ONLY` or above; `IedModel_getControlTargets`
needs `READ_AND_WRITE`.

Public boundary: `src/features/ied_model/service/ied_model_api.h`. Internally split
`service/`/`domain/`/`utils/`/`data/`, per this repo's feature-first convention for
features with real business logic worth isolating: `data/ied_model_scl_loader.c` is the only
file that touches `mxml` (third-party XML) or knows anything about SCL's own XML shape;
`domain/ied_model_usecases.c` is pure logic over an already-built `IedModel*` (libiec61850's
own in-memory model tree), with zero XML/mxml awareness; `utils/` is small stateless XML-attribute
and string-building helpers shared by the loader. `domain/ied_model_types.h` deliberately breaks
from the "framework-free domain layer" convention used elsewhere in this codebase — the domain
vocabulary here genuinely *is* libiec61850's own IEC 61850 model vocabulary
(`IedModel`/`FunctionalConstraint`/`DataAttributeType`/…), not swappable infrastructure.

## 2. Public API surface

All declared in `src/features/ied_model/service/ied_model_api.h`. Every accessor is synchronous,
non-blocking (pure in-memory tree walk), and safe to call from any thread as long as no other
thread is concurrently mutating the same `IedModelHandle` (see §4 — there is no internal locking).

- **`IedModel_loadFromFile(path, iedName, mode, outError)` → `IedModelHandle`**
  Parses the SCL file at `path`, builds the complete `IedModel` for `<IED name=iedName>`, wraps
  it plus the requested `AccessMode` into a heap-allocated handle. Blocking (file I/O + XML
  parse + tree construction) — called once at startup per IED (`orchestration`'s load stage).
  Returns `NULL` and sets `*outError` on failure (`IED_MODEL_ERR_FILE_NOT_FOUND` /
  `_XML_PARSE` / `_IED_NOT_FOUND` / `_UNRESOLVED_TYPE` / `_OUT_OF_MEMORY`). Caller owns the
  handle: `IedModel_release`.

- **`IedModel_wrapDynamicModel(model, iedName, mode)` → `IedModelHandle`**
  Wraps an already-built, caller-owned dynamic `IedModel*` (constructed via
  `iec61850_dynamic_model.h`'s own `IedModel_create`/`LogicalDevice_create`/…, e.g. by
  `ied_model_online_loader` after walking a live device's MMS ACSI directory) in the same handle
  shape `loadFromFile` produces. Every accessor below then behaves identically regardless of
  origin, since they only ever walk `handle->model`. Ownership of `model` transfers to the
  returned handle — `IedModel_release` destroys it identically to a file-loaded one. Returns
  `NULL` (never touching `model`) if `model` is `NULL` or allocation fails. Note: a
  dynamically-wrapped handle has **no SCL `bType` information available** — `daSemantics` is
  always empty on this path (see §3, `IedModelDaSemantic`).

- **`IedModel_listIedNames(path, outError)` → `LinkedList` of `char*`**
  Lists every top-level `<IED name="...">` in the file without building a full model (no
  `DataTypeTemplates` resolution) — lighter weight than `loadFromFile`, used by `orchestration`'s
  IED-name auto-detect (exactly one result ⇒ unambiguous). Zero `<IED>` elements is a valid,
  non-error, empty result. `NULL` + `*outError` only for file/parse failures. Caller owns:
  `LinkedList_destroyDeep(list, free)`.

- **`IedModel_release(handle)`**
  Frees the handle, the wrapped `IedModel*` (`IedModel_destroy`), the owned `iedName` copy, and
  the `daSemantics` array. Safe on `NULL`.

- **`IedModel_getGooseSubscriptionTargets(handle)` → `LinkedList` of `GooseSubscriptionTarget*`**
  One entry per GSEControl block in the model, with SCL-derived addressing when a matching
  `<Communication>` entry exists. Available at every `AccessMode`. Caller owns:
  `LinkedList_destroyDeep(list, IedModel_destroyGooseSubscriptionTarget)`.

- **`IedModel_getReportSubscriptionTargets(handle)` → `LinkedList` of `ReportControlBlockTarget*`**
  One entry per Report Control Block (buffered or unbuffered). Available at every `AccessMode`.
  Caller owns: `LinkedList_destroyDeep(list, IedModel_destroyReportControlBlockTarget)`.

- **`IedModel_getDataSetMemberReferences(handle, datasetReference)` → `LinkedList` of `char*`**
  Ordered member-reference strings for one dataset, index-aligned with the dataset's own wire
  entry order — purely local (never touches the network). Available at every `AccessMode`. Empty
  (never `NULL`) list if `datasetReference` is `NULL` or doesn't resolve. Caller owns:
  `LinkedList_destroyDeep(list, free)`.

- **`IedModel_getDataSetMemberLeafReferences(handle, datasetReference, memberIndex)` → `LinkedList` of `char*`**
  For dataset member `memberIndex`: if its FCDA omitted `daName` (the whole Data Object was
  included, not one leaf DA — "Gap-4 decomposition" territory), returns the ordered leaf
  reference strings for every terminal DA reachable under that DO at its own FC. Empty list
  (never `NULL`) if the member is already leaf-level or on any resolution failure — both mean
  "nothing to decompose." Order relies on a **documented, non-spec-guaranteed assumption**: this
  daemon's own depth-first SCL-tree walk order is assumed to match the wire's actual
  `MMS_STRUCTURE` element order for that entry — callers (`mms_report_client`/`goose_subscriber`)
  must not trust this blindly, see `IedModel_dataAttributeTypeMatchesMmsType` below and §3/§5.
  Caller owns: `LinkedList_destroyDeep(list, free)`.

- **`IedModel_getDataSetMemberSemantics(handle, datasetReference)` / `IedModel_getDataSetMemberLeafSemantics(handle, datasetReference, memberIndex)` → `LinkedList` of `IedModelDaSemantic*`**
  Index-aligned with `getDataSetMemberReferences`/`getDataSetMemberLeafReferences` respectively.
  Reports `IED_MODEL_DA_SEMANTIC_DBPOS` only where the real SCL `bType` was genuinely `"Dbpos"`
  (as opposed to `"Tcmd"`, which collapses to the same wire type — see §3). Always empty for a
  `wrapDynamicModel`-built handle. Caller owns: `LinkedList_destroyDeep(list, free)`.

- **`IedModel_getDataSetMemberLeafWireTypes(handle, datasetReference, memberIndex)` → `LinkedList` of `DataAttributeType*`**
  Index-aligned with `getDataSetMemberLeafReferences` for the same `(datasetReference,
  memberIndex)`. Each element is that leaf's own SCL-declared `DataAttributeType`, read straight
  off the already-built `DataAttribute` node. Feeds `IedModel_dataAttributeTypeMatchesMmsType`
  below. Empty for a `wrapDynamicModel`-built handle. Caller owns: `LinkedList_destroyDeep(list,
  free)`.

- **`IedModel_dataAttributeTypeMatchesMmsType(expected, actual)` → `bool`**
  Cross-checks one leaf's EXPECTED (SCL-declared) `DataAttributeType` against its ACTUAL
  wire-decoded `MmsType`. Pure function, no handle needed. Exists because the count-only
  decomposition fallback cannot catch a same-count-but-different-*order* mismatch between this
  daemon's locally-resolved leaf order and a real device's actual runtime order — confirmed on
  real hardware (a DPC's `Pos` structured attribute had `stVal`/`t` swapped because both possible
  orderings had the same leaf count). Only asserts CONFIDENT, well-established groupings; any
  `DataAttributeType` not explicitly modeled always returns `true` (never a false-positive
  rejection). Called by `mms_report_client`/`goose_subscriber` before trusting a Gap-4
  decomposition zip.

- **`IedModel_getReportableAttributeReferencesForLogicalNode(handle, lnReference)` → `LinkedList` of `char*`**
  For one LN (`"LD/LN"` reference, e.g. a `ReportControlBlockTarget.lnReference`), returns every
  leaf DA at FC=ST or FC=MX reachable under it, same `"LD/LN$FC$DO$DA"` format as
  `getDataSetMemberReferences`. Used by `mms_report_client` to synthesize a dynamic dataset's
  member list for an RCB whose SCL declared no `datSet` (`datSet="Dyn"`). Available at every
  `AccessMode`. Empty list if `lnReference` is `NULL` or doesn't resolve. Caller owns:
  `LinkedList_destroyDeep(list, free)`.

- **`IedModel_destroyReportControlBlockTarget(void*)` / `IedModel_destroyGooseSubscriptionTarget(void*)`**
  `LinkedListValueDeleteFunction`-compatible destructors for the two enriched target structs.

- **`IedModel_getReadTargets(handle)` → `LinkedList` of `char*`**
  Every FC=ST/MX leaf DA reference in the model. Gated: returns an empty list (with a
  `stderr` warning) unless `accessMode >= IED_MODEL_ACCESS_READ_ONLY`.

- **`IedModel_getControlTargets(handle)` → `LinkedList` of `char*`**
  Every FC=CO-capable DataObject reference. Gated: returns an empty list (with a `stderr`
  warning) unless `accessMode == IED_MODEL_ACCESS_READ_AND_WRITE`.

## 3. Per-file breakdown

### `service/ied_model_api.h`
Public boundary — every function above. No struct definitions beyond forward-referencing
`domain/ied_model_types.h`'s public types. Doc comments on this header are the canonical
ownership/gating contract other features are expected to read (not re-derive from `.c`).

### `service/ied_model_api.c`
Thin service layer: wires `data/ied_model_scl_loader.c` and `domain/ied_model_usecases.c`
together, and is the **only** place `AccessMode` gating is enforced (`getReadTargets`/
`getControlTargets` check `handle->accessMode` and short-circuit to an empty list + `stderr`
warning otherwise — every other accessor is available unconditionally, matching the "always
available" tier `REPORT_ONLY` grants).

Key logic beyond simple delegation:
- `adoptDaSemantics` — copies the loader's `LinkedList` of heap-boxed `IedModelDaSemanticEntry*`
  into a flat, handle-owned array (`handle->daSemantics`/`daSemanticCount`) once, at load time,
  then destroys the source list. A linear-scan array rather than a list because
  `lookupDaSemantic` (domain layer) does a small-N linear scan per RCB/target-enable call — not
  a hot path, so no need for a hash map. Degrades to an empty array (never crashes) on OOM.
- `IedModel_loadFromFile` — delegates to `IedModelSclLoader_load`, then builds the handle; on
  any failure path frees the partially-built `daSemanticsList` too (no leak on the error branch).
- `IedModel_wrapDynamicModel` — explicitly documents and enforces that `daSemantics` is always
  empty for this path (no SCL `bType` ever available for a live-discovered model).

### `domain/ied_model_types.h`
Domain vocabulary. Key types:
- **`AccessMode`** — `REPORT_ONLY < READ_ONLY < READ_AND_WRITE`, always hierarchical. Doc
  comment explicitly states the loader always builds the complete model regardless of mode.
- **`IedModelLoadError`** — `OK`, `FILE_NOT_FOUND`, `XML_PARSE`, `IED_NOT_FOUND`,
  `UNRESOLVED_TYPE`, `OUT_OF_MEMORY`.
- **`IedModelDaSemantic`** — currently just `NONE`/`DBPOS`. Exists because
  `IedModelUtils_mapBType` collapses both SCL `"Dbpos"` and `"Tcmd"` bTypes into the same generic
  `IEC61850_CODEDENUM` `DataAttributeType` (same 2-bit wire representation per IEC 61850-7-3,
  different meaning) — this side-channel preserves which one it really was so a consumer
  (`ipc_dispatcher`, via `mms_report_client`/`goose_subscriber`) can label a genuine Dbpos value
  without guessing from the raw bitstring. Room reserved for `IED_MODEL_DA_SEMANTIC_TCMD` later
  — not implemented, no evidence yet it's needed.
- **`IedModelDaSemanticEntry`** — `{DataAttribute* da (borrowed, owned by the IedModel), semantic}`.
  Used both as the loader's boxed `LinkedList` item during parsing and as the handle's flat array
  element type.
- **`ReportControlBlockTarget`** — `{objectReference, buffered, datasetReference, lnReference}`.
  `objectReference` embeds `.BR.`/`.RP.` per `IedConnection_getRCBValues`'s documented convention.
  `lnReference` is always present (every RCB has a parent LN) and is what
  `getReportableAttributeReferencesForLogicalNode` consumes for dynamic-dataset synthesis.
- **`GooseSubscriptionTarget`** — `{objectReference, datasetReference, hasAddress, vlanId,
  vlanPriority, appId, dstMac[6]}`. The addressing fields are only valid when `hasAddress` is
  true (SCL had a matching `<GSE><Address>`).
- **`struct sIedModelHandle`** — the concrete handle type (`IedModel* model`, `AccessMode
  accessMode`, `char* iedName` (owned copy), `IedModelDaSemanticEntry* daSemantics` +
  `daSemanticCount`). Deliberately defined in this shared header rather than a private one —
  every file in the feature needs field access, and opacity toward *external* callers is enforced
  by convention (they only ever hold `IedModelHandle`, an opaque `struct sIedModelHandle*`), not
  by hiding the struct.

### `domain/ied_model_usecases.h` / `ied_model_usecases.c` (795 lines)
Pure logic over an already-built `IedModel*` — zero XML/mxml awareness. Two families of
recursive tree walkers plus the public use-case functions built on them.

**Recursive walkers** (each pair — a "reference-builder" and a "value-extractor" sibling — walks
the identical shape so their outputs stay index-aligned):
- `collectDataAttributesByFc(node, fc, result)` — used by `getReadTargets`. Terminal-izes at the
  first `DataAttribute` node it finds matching `fc` — nested BDAs of a CONSTRUCTED attribute are
  *not* recursed into separately, since the parent DA's own reference already covers them (a
  plain read target reads the whole structure in one MMS read).
- `collectControllableDataObjects(node, result)` — used by `getControlTargets`. Stops recursing
  once it finds a DataObject with an FC=CO child (`DataObject_hasFCData(..., IEC61850_FC_CO)`) —
  reports the controllable DO itself, not nested SDOs individually.
- `collectLeafReferencesByFc(node, fc, pathPrefix, result)` — the Gap-4 decomposition walker.
  Unlike `collectDataAttributesByFc`, this one **does** recurse into a CONSTRUCTED DA's BDA
  children, because on the wire a CONSTRUCTED DA is itself an `MMS_STRUCTURE` — flattening a
  DO-level dataset entry into individual leaf points needs genuinely terminal (basic-typed)
  attributes. FC filtering happens once, at the DataAttribute level (a DO can mix attributes of
  different FCs as direct children — e.g. a DPC's `stVal`/`q`/`t` at ST alongside `Oper`/`SBOw`/
  `Cancel` at CO — so filtering can't happen any higher than the DA itself). Builds the
  `"$"`-joined MMS-variable-name-style path manually (`pathPrefix + "$" + node->name`) rather
  than via `ModelNode_getObjectReference` (which produces the dot-separated ACSI style) — the
  `"$"`-joined style is what every dataset-member reference in this codebase uses, and what
  `ipc_dispatcher`'s quality-pairing logic parses.
- `collectLeafSemanticsByFc` / `collectLeafWireTypesByFc` — identical traversal shape to
  `collectLeafReferencesByFc`, but append a resolved `IedModelDaSemantic` / `DataAttributeType`
  instead of a reference string, guaranteeing index alignment with the leaf-reference list for
  the same `(datasetReference, memberIndex)`.
- `collectLnLeavesByFc(ln, fc, ldName, lnName, result)` — drives `collectLeafReferencesByFc` over
  every direct child (DO) of one LN, for `getReportableAttributeReferencesForLogicalNode`.

**Public use-cases:**
- `IedModelUseCases_getGooseSubscriptionTargets` — walks `handle->model->gseCBs` (a sibling
  linked list, not the tree), builds `"$GO$"`-joined object references and `"$"`-joined dataset
  references, copies `PhyComAddress` fields into the target struct when `gcb->address` is set.
- `IedModelUseCases_getReportSubscriptionTargets` — walks `handle->model->rcbs`, builds
  `.BR./.RP.`-joined object references based on `rcb->buffered`, plus the dataset and parent-LN
  references.
- `IedModelUseCases_getDataSetMemberReferences` — looks up the `DataSet*` via
  `IedModel_lookupDataSet`, joins `entry->logicalDeviceName` + `"/"` + `entry->variableName` per
  `DataSetEntry`.
- `IedModelUseCases_getDataSetMemberLeafReferences` — tokenizes `entry->variableName` on `"$"`
  into `LN`/`FC`/`DO`/`[DA]`. If `daToken` is already present (leaf-level) or `doToken` is
  missing (malformed), returns empty — "nothing to decompose." Otherwise resolves the DO node and
  recurses via `collectLeafReferencesByFc`.
- `resolveTerminalDataAttribute` — resolves an already-leaf-level FCDA's `daToken` down to its
  real terminal `DataAttribute*`. Per IEC 61850-6, `daToken` (the raw SCL `@daName`) can itself be
  a `"."`-separated path into nested BDA/SDO levels (e.g. `"cVal.mag.f"`) — **not** `"$"`-separated
  (that convention is reserved for top-level `LN$FC$DO$daName` segments elsewhere). Splits on
  `"."` and walks `ModelNode_getChild` per segment. Comment notes this is not currently exercised
  by any real fixture in this repo (only flat, undotted `daName` seen so far) but is handled
  correctly regardless.
- `IedModelUseCases_dataAttributeTypeMatchesMmsType` — the confident-groupings-only switch
  described in §2. `IEC61850_BOOLEAN`→`MMS_BOOLEAN`, `IEC61850_TIMESTAMP`→`MMS_UTC_TIME`, the
  bitstring family (`QUALITY`/`CODEDENUM`/`CHECK`/`GENERIC_BITSTRING`/`OPTFLDS`/`TRGOPS`)→
  `MMS_BIT_STRING`, the numeric family (`INT*`/`FLOAT*`/`ENUMERATED`)→`MMS_INTEGER`/
  `MMS_UNSIGNED`/`MMS_FLOAT`, the string family→`MMS_VISIBLE_STRING`/`MMS_STRING`. Everything else
  (`UNKNOWN_TYPE`, `OCTET_STRING_*`, `ENTRY_TIME`, `PHYCOMADDR`, `CURRENCY`, `CONSTRUCTED`) always
  returns `true` — not confident enough to assert, and a false rejection is worse than a missed
  check.
- `IedModelUseCases_getReportableAttributeReferencesForLogicalNode` — splits `lnReference` on the
  first `"/"` into `ldName`/`lnName`, resolves the LN, then calls `collectLnLeavesByFc` for both
  FC=ST and FC=MX.
- `IedModelUseCases_getReadTargets` / `_getControlTargets` — walk every top-level LDevice
  (`handle->model->firstChild`, following `->sibling`) with the two terminal walkers above.

### `data/ied_model_scl_loader.h`
Two entry points: `IedModelSclLoader_load` (full parse + `IedModel` construction, plus
`outDaSemantics`) and `IedModelSclLoader_listIedNames` (top-level `<IED>` enumeration only, no
`DataTypeTemplates` resolution — deliberately does not descend, since `<IED>` elements are always
direct SCL-root children, unlike the general-purpose `mxmlFindElement(..., MXML_DESCEND)` lookup
`_load` itself uses to find one specific IED by name).

### `data/ied_model_scl_loader.c` (989 lines)
The mxml SCL parser — the only file in this feature that knows XML. Structured as a **two-pass
walk over each AccessPoint's LDevice tree**, deliberately: pass 1
(`buildAccessPointStructure`/`buildLogicalDeviceStructure`/`buildLogicalNodeStructure`) builds
every LDevice/LN/DO/DA in the whole IED first; pass 2
(`buildAccessPointReferences`/`buildLogicalDeviceReferences`/`buildLogicalNodeReferences`) then
builds DataSets/ReportControl/GSEControl, which can reference *any* LDevice in the IED including
ones declared later in the file — confirmed directly against libiec61850's own
`complexModel.icd` fixture, where LDevice `Inverter`'s `<DataSet>` references LDevice `Battery`,
declared afterward. A single interleaved top-to-bottom pass would fail to resolve such forward
references.

**Structure construction** (`buildDataObject`/`buildDataAttribute`):
- Recursively walks `DataTypeTemplates`: `LNodeType` → `DOType` (`DA`/`SDO` children) → `DAType`
  (`BDA` children, for `CONSTRUCTED` attributes) → `EnumType`.
- `buildDataAttribute` captures two side-channels during this pass, before the type collapses:
  - **Dbpos semantic**: if raw `bType == "Dbpos"`, records `{da, IED_MODEL_DA_SEMANTIC_DBPOS}`
    into the model-scoped `daSemantics` list (captured from the raw string directly, since
    `IedModelUtils_mapBType` immediately collapses it into the generic `IEC61850_CODEDENUM`
    alongside `"Tcmd"`).
  - **Enum type binding**: if the mapped type is `IEC61850_ENUMERATED`, records
    `{da, enumTypeId}` (the schema's overloaded `type` attribute, here referencing an `EnumType`
    id) into an LN-scoped `enumAttrs` list, consumed later by `lookupEnumOrdinal` when resolving
    a `<DAI>` `Val` override for this attribute.
  - Skips (with a `stderr` warning) any DA whose `bType` doesn't map to a known
    `DataAttributeType` — `IedModelUtils_mapBType` returns `IEC61850_UNKNOWN_TYPE` for anything
    unrecognized, never guessed.
- Unresolved `DOType`/`DAType` references warn and abort that subtree (the DataObject/attribute
  node itself is still created, just left childless) rather than aborting the whole load.

**`<DOI>`/`<DAI>`/`<SDI>`/`<Val>` default-value overrides** (`applyDoiDaiOverrides` →
`applyOverridesUnderDataObject` → `applyValueOverride`):
- `applyOverridesUnderDataObject` recurses into `<SDI>` (structured/array DO override wrapper)
  arbitrarily deep, mirroring `buildDataObject`'s own SDO recursion shape — this was previously a
  bug (only direct `<DAI>` children were visited, silently dropping every nested override; the
  code comment notes tens of thousands of real occurrences across this repo's own sample SCDs).
- `applyValueOverride` type-switches on `da->type` to build the right `MmsValue` — `BOOLEAN`,
  signed/unsigned integer families, `FLOAT32/64`, visible-string family, and a dedicated
  `IEC61850_ENUMERATED` branch. Composite/exotic types (`Struct`, `Quality`, `Timestamp`, …) are
  deliberately skipped, not guessed.
- **Enumerated `<DAI>` `Val` label resolution** (`lookupEnumOrdinal`): resolves a label like
  `"status-only"` against the DA's real `<EnumType>` (found via the `enumAttrs` binding captured
  during the structure pass), matching `<EnumVal>` opaque text and reading its `ord` attribute.
  Falls back to accepting a raw numeric ordinal directly (legal per spec, just uncommon) —
  requiring the **entire** string to be numeric (`strtol` + checking `*end == '\0'`) so a
  non-numeric, unresolvable label never silently becomes ordinal 0 the way a bare `atoi()` would.
  Returns `false` (leave at default, warn) if neither resolves.
- `DataAttribute_setValue` clones the `MmsValue` internally — the locally-built one is always
  `MmsValue_delete`'d right after.

**LDevice/FCDA reference resolution** (`resolveLogicalDeviceByFcdaRef`) — tries **three**
conventions in order, because real-world SCL files are inconsistent about how `FCDA/@ldInst`
maps to an actual LDevice:
1. Bare `LDevice/@inst` match (`IedModel_getDeviceByInst`) — the spec-nominal case.
2. IED-name-prefixed wire form: `ldInstRef` starts with `iedName`, strip that prefix and retry
   as `@inst` — confirmed directly in libiec61850's own `complexModel.icd` fixture
   (`ldInst="ied1Inverter"` against an LDevice whose real `@inst="Inverter"`), vs.
   `sampleModel_with_dataset.icd` which uses the bare form.
3. `LDevice/@ldName` functional-naming match (`IedModel_getDevice`, which looks up by MMS domain
   name — exactly `ldName` when SCL's "functional naming" convention is in effect). Not observed
   in this repo's own sample files but closed proactively as the same bug class as #2, now that
   `ldName` is threaded through from `buildLogicalDeviceStructure`.

**DataSet/FCDA** (`buildDataSets`): builds each `DataSetEntry`'s `"$"`-joined MMS variable name
via `IedModelUtils_buildFcdaVariableName`, resolving the target LDevice through the three-way
fallback above and building its wire name via `buildWireLdName` (`iedName + ldInst`).

**ReportControl** (`buildReportControls` / `resolveRcbRuntimeName`):
- `datSet` is genuinely optional (confirmed against a real Siemens SIPROTEC relay whose
  predefined RCBs omit it, relying on a server-side default) — `mms_report_client` already
  handles a `NULL` dataset reference gracefully.
- `resolveRcbRuntimeName` handles two real-hardware divergences between the SCL-declared RCB
  `name` and its actual runtime/wire name:
  1. **`rptID` suffix mismatch** (confirmed on a real Siemens SIPROTEC relay): some devices
     append a numeric instance suffix to the real object name that shows up in `rptID`
     (`"<LDName>/<LN>$<FC>$<rcbName>"`, the same `"$"`-joined convention used elsewhere) but not
     in the SCL `name` attribute itself — e.g. `name="brcbA"` while the real RCB is `"brcbA01"`.
     When `rptID` has a `"$"`, the substring after the last one is treated as the real name.
  2. **`<RptEnabled max="N">` pre-instantiation** (confirmed on a real ABB REC650, reproduced
     end-to-end with this repo's own simulator): `N > 1` means the server pre-instantiates the
     RCB `N` times (one per potential simultaneous client), each only addressable with a
     zero-padded 2-digit suffix — e.g. `name="rcb_A"` with `max="5"` is only ever
     `"rcb_A01".."rcb_A05"` on the wire, **never** bare `"rcb_A"`. This daemon is always exactly
     one client, so it always targets instance `"01"` — no retry against other instances if `01`
     is already claimed elsewhere.
  Always returns a newly heap-allocated string (caller must `free`) in every case, unifying
  ownership regardless of whether a suffix was actually appended.

**GSEControl + Communication addressing** (`buildGseControls` / `findCommunicationGseAddress` /
`attachPhyComAddress` / `parseMacAddress`):
- `GSEControl`'s `datSet` is kept **required** (unlike `ReportControl`'s, made optional above) —
  every real `GSEControl` sample across this repo's own SCDs (86/86) populates it, so loosening
  would be speculative.
- `findCommunicationGseAddress` manually iterates `<Communication>/<SubNetwork>/<ConnectedAP>/<GSE>`
  (not a single `mxmlFindElement` call, since several `ConnectedAP`/`GSE` entries can exist),
  matching on `iedName`+`apName` then `ldInst`+`cbName`.
- **`<GSE>` `MinTime`/`MaxTime`**: live under the same `<GSE>` as `<Address>` in `<Communication>`,
  not on `<GSEControl>` itself — so this lookup happens *before* `GSEControlBlock_create`, since
  `minTime`/`maxTime` are constructor-only parameters (no post-create setter). A prior version of
  this code looked the address up only after construction (purely to attach it), always passing
  library-default `-1,-1` regardless of what the file specified — fixed once confirmed real files
  populate these values.
- **Hex-parsed VLAN-ID/APPID** (`attachPhyComAddress`): parsed with `strtoul(text, NULL, 16)`,
  never base-0 auto-detection — VLAN-ID/APPID are hex strings per IEC 61850-8-1, and base-0
  autodetection would treat a leading-zero, non-`"0x"`-prefixed value as *octal*, silently
  mis-parsing any real value containing an `'8'`/`'9'` or hex letter (confirmed against real
  `APPID` values like `"000A"` in this repo's own sample SCDs — previously silently became 0).
  `VLAN-PRIORITY` is decimal (`atoi`).
- `parseMacAddress` — `sscanf` with `"%2x-%2x-%2x-%2x-%2x-%2x"`, dash-separated only; requires
  all 6 groups to parse or the whole thing fails (see §5 for non-dash format limitation).
- Absence of a matching `<Communication>` entry is not an error — typical for plain `.icd` files
  with no network config assigned yet; the GoCB is still built, just with `hasAddress = false`.

**Vendor `<Private>` control-block-storage detection** (`containsPrivateControlBlockStorage`):
detects a real Siemens SIPROTEC export pattern where RCB/GoCB elements aren't literal SCL at all
— they're HTML-escaped text embedded inside `<Private type="Siemens-ControlBlockStorage">`
wrapper elements, meant to be "activated" into real elements only once assigned in a project. Such
a file parses without any XML error and silently produces a structurally valid but completely
empty model (zero RCB/GoCB/DataSet). This function is **detection-only** — parsing the escaped
payload is out of scope (vendor-specific) — it just recursively scans for a `<Private>` element
whose `type` attribute *contains* (`strstr`, not exact match, to also catch other vendor-prefixed
variants) `"ControlBlockStorage"`, and emits a `stderr` diagnostic explaining why the model is
empty instead of leaving the operator with no clue.

**SCL-root resolution** (`loadSclRoot`): finds the `<SCL>` element specifically rather than
trusting "the first element-typed child" of the parsed tree. The vendored Mini-XML (`mxml`) has
no distinct comment node type — a leading top-level `<!--comment-->` (confirmed present in a real
ABB export) is itself represented as an `MXML_ELEMENT` node whose `mxmlGetElement()` returns raw
`"!--...--"` text. Without this fix, `firstElementChild()` alone would silently treat that comment
as the SCL root, leaving every subsequent lookup searching inside an empty node and surfacing as a
spurious `IED_MODEL_ERR_IED_NOT_FOUND` rather than an XML parse error — the real cause was easy to
miss. Every other loop in this file already filters by explicit tag name via `isElement()` and
was unaffected; this was the one place that didn't.

**Entry points**: `IedModelSclLoader_load` runs pass 1 then pass 2 over every `<AccessPoint>`,
then runs the vendor-Private diagnostic if the resulting model has zero RCBs/GoCBs/DataSets.
`IedModelSclLoader_listIedNames` shares `loadSclRoot` but does a flat, non-descending scan for
`<IED>` children only.

### `utils/ied_model_utils.h` / `ied_model_utils.c`
Stateless helpers, no state, no third-party calls beyond `mxml` attribute/opaque-text accessors:
- `attrOrDefault`/`attrRequired`/`attrBool`/`attrInt` — thin wrappers over
  `mxmlElementGetAttr`, with `attrBool` treating `"true"` or `"1"` as true (nothing else) and
  `attrInt` using plain `atoi` (non-numeric garbage silently becomes 0 — see §5).
- `buildLnName(prefix, lnClass, inst)` — synthesizes `prefix + lnClass + inst` (empty
  prefix/inst for LN0 gives `"LLN0"`).
- `mapBType` — the full SCL `bType` → `DataAttributeType` table (documented in full in §3 above
  under structure construction). Two explicit approximations: `INT24` (signed) has no direct
  libiec61850 counterpart and maps to `IEC61850_INT32`; `ObjRef` has no counterpart and maps to
  `IEC61850_VISIBLE_STRING_129` (object references are bounded strings per spec). Unrecognized
  strings return `IEC61850_UNKNOWN_TYPE`.
- `buildTrgOps`/`buildOptFlds` — build the `TRG_OPT_*`/`RPT_OPT_*` bitsets from `<TrgOps>`/
  `<OptFields>` boolean attributes.
- `buildFcdaVariableName(ldName, lnClass, lnInst, prefix, fc, doName, daName)` — builds the
  `"$"`-joined MMS variable name `DataSetEntry_create` expects (e.g.
  `"ied1Inverter/LLN0$ST$Mod$q"`); `daName == NULL` omits the trailing `$daName` segment
  (whole-DO reference, the Gap-4 decomposition candidate).

## 4. Threading & concurrency model

`ied_model` itself spawns no threads and holds no internal locks. Every accessor is a pure,
synchronous, in-memory tree walk (or, for `loadFromFile`, blocking file I/O + parse). The model
is intended to be **built once at IED-connect time, then treated as effectively read-only** for
the rest of the handle's lifetime — every downstream caller only ever reads via the accessors
above (no mutating API is exposed publicly beyond `IedModel_release`).

Concretely: `orchestration`'s load stage calls `IedModel_loadFromFile` once, synchronously, on
whatever thread is running `Orchestration_run`. The resulting `IedModelHandle` is then handed to
`mms_report_client` and `goose_subscriber`, which each read from it — `mms_report_client`'s
supervisor thread and `goose_subscriber`'s reception thread may both be calling into this handle
concurrently (e.g. `getDataSetMemberReferences`/`getReportableAttributeReferencesForLogicalNode`
during RCB enable, `getDataSetMemberLeafReferences` during Gap-4 decomposition on report/frame
arrival). This is safe **only because nothing mutates the handle after load** — there is no
lock protecting `handle->model` or `handle->daSemantics` because none is needed under that
invariant. If a future caller ever needed to reload or mutate a live handle's model, this would
need locking added; today it doesn't exist.

`IedModel_release` must only be called once the caller is certain no other thread still holds a
reference to the handle (standard single-owner-at-teardown assumption, same as elsewhere in this
codebase — `orchestration`'s fail-hard reverse-order teardown enforces this).

## 5. Known limitations / deliberate scope boundaries

Explicitly called out in code comments as deferred/not hardened:

- **Duplicate `LDevice/@inst`** — not handled; behavior with two LDevices sharing the same
  `@inst` within one IED is undefined (`IedModel_getDeviceByInst` returns whichever the
  underlying dynamic-model lookup finds first).
- **`DAI/@ix` array indices** — not handled; array-typed DAI overrides are not specially
  resolved by index.
- **`<Val sGroup="N">` overrides** (SCL setting-group-scoped default values) — not handled;
  `applyValueOverride` always applies the one `<Val>` it finds regardless of `sGroup`.
- **Dotted FCDA shorthand** — not handled as a *dataset-reference* convention (though
  `resolveTerminalDataAttribute`, used elsewhere for semantics/wire-type lookup, does correctly
  split an already-leaf-level `daName` on `"."` for nested BDA/SDO paths — see §3).
- **Non-dash MAC address formats** — `parseMacAddress` only accepts `"%2x-%2x-%2x-%2x-%2x-%2x"`
  (dash-separated); colon-separated or other formats fail to parse (GoCB address left unset for
  that field's data, since `haveMac` stays false).
- **`INT24` and `ObjRef`** — deliberate, documented approximations in `IedModelUtils_mapBType`
  (see §3) rather than exact representations; no libiec61850 counterpart exists for either.
- **Vendor `<Private>`-embedded control blocks** (Siemens SIPROTEC `ControlBlockStorage`
  pattern) — detected and diagnosed (`containsPrivateControlBlockStorage`), but the escaped
  payload itself is never parsed; such an IED loads with zero RCB/GoCB/DataSet targets.
- **`attrInt`'s `atoi`-based parsing** — non-numeric garbage silently becomes `0`, same posture
  as the rest of this loader's "degrade rather than crash" philosophy, but worth noting as not a
  strict validator.
- **Gap-4 decomposition's wire-order assumption is fundamentally unverifiable locally** — the
  loader's own depth-first SCL-tree traversal order is *assumed* to match a real device's actual
  runtime `MMS_STRUCTURE` element order; this is documented in the header as "an assumption, not
  a certainty," which is exactly why `IedModel_dataAttributeTypeMatchesMmsType` exists as a
  defensive cross-check for the consuming features rather than something `ied_model` itself can
  fully guarantee.
- **`IED_MODEL_DA_SEMANTIC_TCMD`** — reserved in the enum but not implemented; only `Dbpos` is
  currently distinguished from the generic CODEDENUM collapse.
- **A `wrapDynamicModel`-built handle has zero `daSemantics`** (no SCL `bType` ever available
  over the wire) — every semantic-lookup accessor degrades to `IED_MODEL_DA_SEMANTIC_NONE` for
  online-discovered models. Documented as an accepted limitation, not a regression.

## 6. Cross-feature dependencies

**Calls into:**
- `mxml` (third-party, vendored) — exclusively from `data/ied_model_scl_loader.c`; no other file
  in this feature touches it.
- `libiec61850`'s `iec61850_model.h`/`iec61850_dynamic_model.h`/`iec61850_common.h`/`mms_common.h`/
  `mms_value.h` — the in-memory model tree, `ModelNode`/`DataObject`/`DataAttribute`/`DataSet`/
  `ReportControlBlock`/`GSEControlBlock` construction and traversal, `MmsValue` for `<Val>`
  override construction. Per the "libiec61850 is mandatory" Hard Rule, no hand-rolled parsing of
  the model shape itself.

**Called by:**
- `goose_subscriber` — `IedModel_getGooseSubscriptionTargets` for its subscription list,
  `IedModel_getDataSetMemberReferences` to resolve a dataset member's reference (GOOSE frames
  never carry references on the wire).
- `mms_report_client` — `IedModel_getReportSubscriptionTargets` for its RCB list,
  `IedModel_getDataSetMemberReferences`/`_getDataSetMemberLeafReferences`/
  `_getDataSetMemberLeafWireTypes`/`IedModel_dataAttributeTypeMatchesMmsType` for Gap-4
  decomposition, `IedModel_getReportableAttributeReferencesForLogicalNode` for dynamic-dataset
  synthesis on RCBs with `datSet="Dyn"`.
- `orchestration` — `IedModel_loadFromFile` (or, on the SCL-fetch-from-file path,
  `Orchestration_runFromLocalFile`'s equivalent) as its "load" pipeline stage;
  `IedModel_listIedNames` for optional IED-name auto-detection.
- `ied_model_online_loader` — builds a dynamic `IedModel*` from live ACSI directory services,
  then calls `IedModel_wrapDynamicModel` to hand it back in the exact same handle shape every
  other accessor already knows how to walk. This is the one narrow exception to "No over-the-wire
  tree discovery" — `ied_model` itself has no awareness this happened; it just wraps whatever
  `IedModel*` it's given.

**Must never do** (Hard Rules):
- Never hand-roll GOOSE/MMS/SCL parsing outside libiec61850's own model APIs + `mxml` for XML.
- Never perform over-the-wire tree discovery itself (that's `ied_model_online_loader`'s sole,
  narrow, explicitly-invoked job — `ied_model` only ever consumes whatever `IedModel*` it's
  handed, whether SCL-parsed or dynamically built).
- Never guess IEC 61850 semantics it isn't confident about — see `mapBType`'s conservative
  `IEC61850_UNKNOWN_TYPE` fallback and `dataAttributeTypeMatchesMmsType`'s "unmodeled always
  matches" posture, both explicit applications of the "if unsure of exact IEC 61850 semantics,
  say so, don't guess" Hard Rule.

## 7. Tests

**`tests/ied_model/`** (strict unit, Unity, run via `cd tests && make run`):
- `test_ied_model_utils.c` (26 `RUN_TEST`s) — every `ied_model_utils.c` function in isolation:
  `attrBool`/`attrInt`/`attrOrDefault`/`attrRequired` edge cases (absent, default, non-truthy,
  non-numeric garbage), `buildLnName`, `buildFcdaVariableName` (with/without `daName`,
  null prefix/lnInst), `buildTrgOps`/`buildOptFlds` bit-setting, `mapBType`'s documented
  approximations and common-primitive mappings, `mapBType`'s unknown-type fallback. Links only
  `ied_model_utils.c` + `mxml` — no `libiec61850` model calls at all.
- `test_ied_model_usecases.c` (35 `RUN_TEST`s) — builds real `IedModel` trees directly via the
  dynamic-model API (`IedModel_create`/`LogicalDevice_create`/… — no SCL/mxml involved) and
  exercises every `IedModelUseCases_*` function: Gap-4 decomposition (flat-DO decompose, already-
  leaf-level no-op, FC filtering excludes sibling FCs, CONSTRUCTED-attribute recursion, negative/
  out-of-range/NULL index handling), dataset member reference ordering/multi-entry, GOOSE/report
  target construction (correct reference shape, `.RP.` for unbuffered, NULL dataset when GCB/RCB
  has none, PhyComAddress population), read/control target FC filtering,
  `getReportableAttributeReferencesForLogicalNode` (LN/LD resolution failures, no-slash-in-reference,
  CONSTRUCTED-attribute recursion, ST/MX-only scoping to the given LN), and
  `dataAttributeTypeMatchesMmsType`'s confident-match/confident-mismatch/unmodeled-always-matches
  cases. Links `ied_model_usecases.c` + real `libiec61850`/`hal` — no `mxml`.
- `test_ied_model_api.c` (11 `RUN_TEST`s) — the **one file-I/O case** in this feature's unit
  tests (per CLAUDE.md's "two self-contained temp-file cases" carve-out): writes a minimal
  self-contained SCL fixture to a `mkstemp`'d temp file per test, proving `IedModel_loadFromFile`'s
  own *wiring* (open file → delegate to loader → populate handle) — explicitly **not** the
  loader's real-world parsing breadth, which the doc comment says is the E2E test's job. Also
  covers `listIedNames` (zero/one/two `<IED>` fixtures) and `AccessMode` gating
  (`REPORT_ONLY`/`READ_ONLY`/`READ_AND_WRITE` tiers, constructing a handle directly over an
  in-memory model to isolate gating logic from loader correctness). Links the full stack
  (`service` + `data` + `domain` + `utils`) plus `libiec61850`/`hal`/`mxml`.

Confirms CLAUDE.md's claim: **the SCL loader's real-world parsing correctness is deliberately
NOT unit-tested** beyond that one minimal wiring fixture — it's proven end-to-end instead. No
unit test exercises `IedModelSclLoader_load`'s DataTypeTemplates resolution, DOI/DAI/SDI override
application, RCB runtime-name resolution, GSEControl/Communication addressing, or any of the
hardening fixes in §3 directly — that's all `integration_tests/`' job. Also notably: **no unit or
E2E test exercises `IedModel_getDataSetMemberSemantics`/`_getDataSetMemberLeafSemantics` (the
Dbpos-semantic accessors) at all** — `IedModelDaSemantic`/Dbpos-labeling has fixture support
(`fixtures/breaker1.cid` contains `Dbpos`-typed attributes) but no test asserts on the resulting
semantic value.

**`integration_tests/ied_model/`** (E2E, Unity, real `src/` code, run via
`cd integration_tests/ied_model && make run`, no `sudo`):
- `e2e_test_ied_model.c` (13 `RUN_TEST`s) against four self-authored fixture files in
  `fixtures/`:
  - `breaker1.cid` — the main substantial fixture: `test_loadsFixtureSuccessfully`,
    `test_readTargets_matchExpectedCount_andExcludeConfigAndControlAttributes`,
    `test_controlTargets_includeOnlyThePosDataObject`,
    `test_reportSubscriptionTarget_resolvesCorrectReference`,
    `test_gooseSubscriptionTarget_resolvesCorrectReference`,
    `test_gooseControlBlock_hasPhyComAddressAttached_fromCommunicationSection`,
    `test_reportOnlyMode_deniesReadAndControlTargets`.
  - `hardening_edge_cases.cid` — `test_hardening_hexAppidVlanId_parsedCorrectly`,
    `test_hardening_ldNameFunctionalNaming_resolvesFcdaAndRoundTrips`,
    `test_hardening_nonNumericEnumOverride_resolvesRealOrdinal`,
    `test_hardening_sdiWrappedOverride_appliesNestedValue` — directly exercises the four
    hardening fixes called out in CLAUDE.md and §3 above (hex VLAN/APPID, `ldName` functional
    naming, enum-ordinal resolution, nested `<SDI>` overrides).
  - `leading_comment.icd` — `test_leadingComment_doesNotDerailSclRootResolution`, proving the
    `loadSclRoot` comment-vs-`<SCL>`-root fix.
  - `private_only.icd` — `test_privateOnly_loadsSuccessfully_withEmptyTargets`, proving the
    vendor `<Private>` control-block-storage detection path degrades gracefully (empty targets,
    diagnostic warning, no crash) rather than erroring.
- Not covered even at the E2E level: duplicate `LDevice/@inst`, `DAI/@ix` array indices,
  `<Val sGroup="N">` overrides, dotted FCDA shorthand, non-dash MAC formats (all §5 limitations),
  and the Dbpos/`IedModelDaSemantic` accessors (as noted above).
