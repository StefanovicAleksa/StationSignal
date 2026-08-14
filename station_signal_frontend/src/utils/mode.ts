/**
 * Which deployment mode this bundle was built for.
 *
 * `VITE_STATION_SIGNAL_MODE` is baked in at build time by `deploy/setup.sh dev|prod` (see the
 * parent repo's `deploy/README.md`), mirroring the mode the API and daemon run in.
 * `import.meta.env.DEV` is OR'd in so `pnpm dev` and the Vitest run are always treated as dev
 * without anyone having to set the variable locally.
 *
 * Lives in its own module rather than inside `logger.ts` because it now gates UI as well as
 * console output — the Settings page's Advanced section — and both need to agree on one answer.
 *
 * It resolves at BUILD time, so there is nothing to toggle at runtime and changing an installed
 * box's mode only affects this half after a rebuild.
 *
 * What build-time resolution does and does not buy you, precisely — because the difference matters
 * and is easy to overstate. Vite substitutes both env reads, so this const folds to a literal and
 * a call guarded by `if (!isDevMode) return` compiles away to nothing (a prod bundle really does
 * contain `debug(...e){}` with an empty body). A Vue template `v-if` is NOT the same thing: it
 * compiles to a runtime conditional in the render function, so a `v-if="isDevMode"` section is
 * reliably *not rendered* in prod but its markup and message keys are still present in the
 * shipped JavaScript. Do not treat "it's dev-gated in the UI" as "it isn't in the bundle".
 *
 * Which is why the API gates the matching endpoint independently (`cfg.Mode == ModeDev`, and it
 * does not register the route at all in prod). That is the actual boundary; this flag decides what
 * a technician is shown, not what the box will do.
 */
export const isDevMode = import.meta.env.VITE_STATION_SIGNAL_MODE === 'dev' || import.meta.env.DEV
