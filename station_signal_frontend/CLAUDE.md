# frontend

## Purpose
What the technician actually looks at on-site: live GOOSE/MMS data, device/scan controls (see
`../CLAUDE.md` for how this fits into the whole product). Talks to `station_signal_api` only —
never to `station_signal_daemon` directly, and never over anything but the REST/WS surface that
API exposes.

This file governs `frontend/` only. `../station_signal_api/CLAUDE.md` remains the authority on
the API's own internals — nothing here should re-derive or fork that.

## Commands
- `pnpm dev` / `pnpm build` / `pnpm preview`
- `pnpm type-check` (`vue-tsc --build`) — **the main safety net.** Message catalogs and component
  emit signatures are both type-enforced, so most breakage is a compile error rather than a
  runtime surprise. Run it before assuming a refactor landed.
- `pnpm test:unit` (Vitest, jsdom). Specs live flat in `src/__tests__/`, not colocated.

## Current State
Implemented: Vue 3 + TypeScript + Vue Router + Pinia + Vitest + Tailwind v4. Four views (Scan,
Devices, Reports, Settings) over three Pinia stores (`devices`, `scan`, `settings`), a thin
`services/` layer of `fetch`/WebSocket wrappers, and `components/ui/` primitives. Dependency
surface is deliberately minimal — no axios (raw `fetch`), no date library, no UI kit, no i18n
library. Keep it that way absent a real reason.

## Internationalization
Every user-facing string goes through `t()` from `src/i18n`. Two locales: English and Serbian
(Latin script). There is no i18n library — `src/i18n/index.ts` is a small singleton composable
deliberately mirroring `src/composables/useTheme.ts` (module-scoped `ref` + `localStorage`), and
the language toggle sits next to the theme toggle in `AppShell`.

- **`src/i18n/messages/en.ts` is the source of truth.** `sr.ts` is typed as `Messages` (derived
  from `typeof en` in `src/i18n/types.ts`), so a key in one and not the other is a `vue-tsc`
  error, not a blank string at runtime. `t()`'s key parameter is a generated union of dot-paths,
  so a renamed key can't survive as a silently-wrong lookup either.
- What types *can't* catch is a translation that drops an interpolation slot — `i18n.spec.ts`
  asserts placeholder parity between locales for exactly that reason.
- **`t()` reads `locale.value`, so it only re-evaluates where that's tracked**: fine directly in a
  template, fine inside `computed`. A bare `const label = t(...)` in setup scope freezes at the
  locale active when the component mounted, so script-side label maps must be `computed`.
- Store-side calls (error fallbacks) are deliberately snapshot-at-construction — an error message
  keeps the wording it had when the failure actually happened.
- IEC 61850 vocabulary (GOOSE, MMS, RCB, LN, SCL, ICD/CID, CIDR, host, port) stays untranslated in
  every locale; that's how substation engineers write it in both languages.
- Device phase labels live in one place, `composables/useDevicePhaseLabel.ts` — four call sites
  previously each carried their own literal map, and had already drifted.

## Connect shortcuts
Clicking Connect opens the LN-category picker. Two modifier shortcuts skip it, modelled as a
`ConnectPreset` (`utils/connectPreset.ts`) rather than a boolean, and surfaced via
`ui/ShortcutHint.vue` plus button tooltips — the earlier boolean was undiscoverable in the UI and
read as "connect unfiltered" at every call site while meaning the opposite.

| Click | Preset | `lnCategories` sent |
|---|---|---|
| plain | `ask` | whatever the picker returns |
| Shift | `default` | `DEFAULT_LN_CATEGORIES` (`['CONTROL','OTHER']`) |
| Ctrl/Cmd | `all` | omitted — unfiltered |

`Control + Other`, not Control alone, because real devices parent their control blocks on `LLN0`,
which classifies as Other; a Control-only default would connect and show nothing.

## Two Things That Are Derived, Not Tracked
Both replaced hand-maintained bookkeeping that went stale in ways nothing detected:
- **Scan row state.** `ScanResultsTable` reads the devices store per row to decide Connect vs.
  View vs. a phase badge. It previously *deleted* a row on a successful connect, but the daemon's
  scan worker keeps a per-scan seen-set and never republishes a host it has already reported — so
  the row was gone for the life of that scan and disconnecting left no way back to the device.
  Deriving it means `stopDevice` restores the Connect button for free.
- **Effective LN categories.** The API attaches a session to an already-running device at the same
  `host:mmsPort` and keeps the *creator's* params, so a second operator's category choice is
  silently discarded. `stores/devices.ts`'s `applyEffectiveCategories` compares requested against
  the `lnCategories` the response reports and flags the mismatch, and `DeviceReportPanel` says so.
  Per-viewer narrowing is a client-side filter over that one shared stream (each data point
  carries its own `category`) — **never** a second daemon connection to the same IED; see the
  daemon's `CLAUDE.md` `device_manager` bullet for why that corrupts both clients.

## The API This Frontend Talks To
Read `../station_signal_api/docs/FRONTEND_API_GUIDE.md` **before writing any client code** — it
is the full, self-contained wire contract (every REST endpoint, both WS endpoints, every error
code, worked flows) confirmed directly against that API's actual code. Treat what's below as
orientation, not a substitute for reading it.

- Base URL is this API's own HTTP address (e.g. `http://<host>:8080`) — **never** the daemon's
  ports (8767/8766/9000-9999). This frontend has no reason to know those exist.
- Two REST resource groups, `/api/devices` and `/api/scans` (`POST` to start, `DELETE /{id}` to
  stop, `GET` to list), plus `GET /api/health`.
- Two WS endpoints for live data: `/ws/devices/{deviceId}` (one connection per watched
  device) and `/ws/scans` (one shared connection, accepts and idles even with no scan active).
  Both are push-only — never send anything on them.
- Every error response is `{"error":{"code":"...","message":"...","stage":null,"detail":null}}`
  — the guide's §5 has the full code → HTTP-status table, including which ones (`DAEMON_UNREACHABLE`)
  deserve a retry-with-backoff UI treatment rather than a hard failure.
- No auth, no TLS anywhere in this stack (loopback/local-substation-network trust model per
  the top-level `../CLAUDE.md`) — don't build auth flows this system doesn't have.

## Hard Rules (with reasons)
- **Never connect to the daemon's own ports.** This frontend's only backend is
  `station_signal_api` — even though the daemon's raw wire format is technically reachable if
  you knew the ports, going around the API defeats the entire reason it exists (privilege
  separation, connection multiplexing, crash re-arm). If something feels missing from the
  API's surface, that's an API change to make deliberately, not something to work around here.
- **Treat `FRONTEND_API_GUIDE.md` as an external stable contract to consume, not guess at.**
  If the API's response shape ever seems to need a change, that's a `station_signal_api` change
  first — not something to paper over with ad-hoc parsing on this side.
