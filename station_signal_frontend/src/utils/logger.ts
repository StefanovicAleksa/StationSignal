/**
 * Console logging, gated on the build's deployment mode.
 *
 * `VITE_STATION_SIGNAL_MODE` is baked in at build time by `deploy/setup.sh dev|prod` (see the
 * parent repo's `deploy/README.md`), mirroring the mode the API and daemon run in. In prod
 * `debug()` is a no-op, so diagnostic chatter never reaches a technician's devtools console;
 * `warn()`/`error()` always pass through, since those report something actually going wrong.
 *
 * `import.meta.env.DEV` is OR'd in so `pnpm dev` and the Vitest run are always verbose without
 * anyone having to set the variable locally.
 */
const isDevMode = import.meta.env.VITE_STATION_SIGNAL_MODE === 'dev' || import.meta.env.DEV

export const logger = {
  debug(...args: unknown[]): void {
    if (!isDevMode) return
    console.debug(...args)
  },
  warn(...args: unknown[]): void {
    console.warn(...args)
  },
  error(...args: unknown[]): void {
    console.error(...args)
  },
}
