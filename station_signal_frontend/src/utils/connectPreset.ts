import { DEFAULT_LN_CATEGORIES, type LnCategory } from '@/types/api'

/**
 * How a Connect click decided which LN categories to subscribe to:
 *
 *  - `ask`     — plain click: open the category picker (the default, and the only discoverable path).
 *  - `default` — Shift held: skip the picker, use DEFAULT_LN_CATEGORIES (Control + Other).
 *  - `all`     — Ctrl/Cmd held: skip the picker, subscribe unfiltered.
 *
 * This replaced a bare `bypassCategoryModal` boolean, which could only express the first two and
 * read as "connect unfiltered" at every call site while actually meaning the opposite.
 */
export type ConnectPreset = 'ask' | 'default' | 'all'

/**
 * Ctrl is checked before Shift, so a Ctrl+Shift click resolves to `all` rather than depending on
 * evaluation order. `metaKey` is accepted alongside `ctrlKey` for Cmd on macOS.
 */
export function connectPresetForEvent(event: MouseEvent): ConnectPreset {
  if (event.ctrlKey || event.metaKey) return 'all'
  if (event.shiftKey) return 'default'
  return 'ask'
}

/**
 * The `lnCategories` value a non-`ask` preset connects with. `undefined` means unfiltered — the
 * same value ConnectCategoryModal emits for "Connect to all categories", so the shortcut and the
 * picker can't drift apart.
 */
export function categoriesForPreset(preset: Exclude<ConnectPreset, 'ask'>): LnCategory[] | undefined {
  return preset === 'all' ? undefined : DEFAULT_LN_CATEGORIES
}
