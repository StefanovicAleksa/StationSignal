/**
 * Console logging, gated on the build's deployment mode.
 *
 * In prod `debug()` is a no-op, so diagnostic chatter never reaches a technician's devtools
 * console; `warn()`/`error()` always pass through, since those report something actually going
 * wrong.
 *
 * The mode flag itself lives in `./mode` — it gates UI as well as logging now, and both have to
 * agree on one answer. See that module for how it is resolved and why it is build-time.
 */
import { isDevMode } from './mode'

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
