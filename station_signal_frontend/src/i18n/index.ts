import { ref } from 'vue'

import { en } from './messages/en'
import { sr } from './messages/sr'
import type { MessageKey, MessageParams, Messages } from './types'

export type { MessageKey, MessageParams, Messages } from './types'

const STORAGE_KEY = 'station-signal:locale'

export const LOCALES = [
  { value: 'en', label: 'English', short: 'EN' },
  { value: 'sr', label: 'Srpski', short: 'SR' },
] as const

export type Locale = (typeof LOCALES)[number]['value']

const catalogs: Record<Locale, Messages> = { en, sr }

function isLocale(value: string | null): value is Locale {
  return LOCALES.some((entry) => entry.value === value)
}

function readStoredLocale(): Locale | null {
  const stored = localStorage.getItem(STORAGE_KEY)
  return isLocale(stored) ? stored : null
}

// Only ever consulted when nothing is stored — an explicit choice always wins, and unlike theme
// there's no OS-level "change" event worth following afterwards.
function browserLocale(): Locale {
  return navigator.language?.toLowerCase().startsWith('sr') ? 'sr' : 'en'
}

function applyLocale(next: Locale) {
  document.documentElement.lang = next
}

// Module-scoped singleton, same shape as useTheme() — every component and store shares one ref,
// so switching locale re-renders everything at once.
const locale = ref<Locale>(readStoredLocale() ?? browserLocale())
applyLocale(locale.value)

function lookup(catalog: Messages, key: string): string | undefined {
  let node: unknown = catalog
  for (const segment of key.split('.')) {
    if (typeof node !== 'object' || node === null) return undefined
    node = (node as Record<string, unknown>)[segment]
  }
  return typeof node === 'string' ? node : undefined
}

/**
 * Translates `key` in the active locale, substituting any `{placeholder}` slots from `params`.
 *
 * Reads `locale.value`, so calling this inside a template or a `computed` re-evaluates on a
 * locale change. A plain `const label = t(...)` in setup scope does NOT — wrap those in
 * `computed`. Store-side calls are deliberately snapshot-at-construction: an error message keeps
 * the wording it had when the failure actually happened.
 *
 * Falls back to English, then to the key itself, so a gap can never render as blank. `sr.ts` is
 * type-checked against `en.ts`, so that path only exists as runtime belt-and-braces.
 */
export function t(key: MessageKey, params?: MessageParams): string {
  const template = lookup(catalogs[locale.value], key) ?? lookup(en, key) ?? key
  if (!params) return template
  return template.replace(/\{(\w+)\}/g, (whole, name: string) =>
    name in params ? String(params[name]) : whole,
  )
}

export function setLocale(next: Locale) {
  locale.value = next
  localStorage.setItem(STORAGE_KEY, next)
  applyLocale(next)
}

export function useI18n() {
  return { locale, t, setLocale, LOCALES }
}
