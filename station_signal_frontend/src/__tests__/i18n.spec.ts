import { describe, it, expect, beforeEach } from 'vitest'

import { t, setLocale, useI18n, LOCALES } from '@/i18n'
import { en } from '@/i18n/messages/en'
import { sr } from '@/i18n/messages/sr'

function flattenKeys(source: unknown, prefix = ''): string[] {
  if (typeof source !== 'object' || source === null) return []
  return Object.entries(source).flatMap(([key, value]) => {
    const path = prefix ? `${prefix}.${key}` : key
    return typeof value === 'string' ? [path] : flattenKeys(value, path)
  })
}

function placeholdersIn(value: string): string[] {
  return [...value.matchAll(/\{(\w+)\}/g)].map((match) => match[1] ?? '').sort()
}

function flattenEntries(source: unknown, prefix = ''): [string, string][] {
  if (typeof source !== 'object' || source === null) return []
  return Object.entries(source).flatMap(([key, value]): [string, string][] => {
    const path = prefix ? `${prefix}.${key}` : key
    return typeof value === 'string' ? [[path, value]] : flattenEntries(value, path)
  })
}

describe('i18n catalogs', () => {
  // `sr.ts` is typed as `Messages`, so this is already a compile-time guarantee — asserted here
  // too so a locale gap fails as a readable test name rather than a wall of tsc output.
  it('sr covers exactly the same keys as en', () => {
    expect(flattenKeys(sr).sort()).toEqual(flattenKeys(en).sort())
  })

  // Types can't catch this one: a translation that drops or renames an interpolation slot type-
  // checks fine and then renders a literal "{host}" (or silently loses the value) at runtime.
  it('every translated string carries the same placeholders as its English original', () => {
    const srByKey = new Map(flattenEntries(sr))
    for (const [key, value] of flattenEntries(en)) {
      expect({ key, placeholders: placeholdersIn(srByKey.get(key) ?? '') }).toEqual({
        key,
        placeholders: placeholdersIn(value),
      })
    }
  })

  it('exposes exactly the locales the toggle offers', () => {
    expect(LOCALES.map((entry) => entry.value)).toEqual(['en', 'sr'])
  })
})

describe('t()', () => {
  beforeEach(() => {
    localStorage.clear()
    setLocale('en')
  })

  it('resolves a nested dot-path', () => {
    expect(t('scan.status.active')).toBe(en.scan.status.active)
  })

  it('substitutes named placeholders', () => {
    expect(t('connectPrompt.passwordLabel', { host: '10.0.0.7' })).toBe('10.0.0.7 requires a password')
  })

  it('leaves a placeholder untouched when no value is supplied for it', () => {
    expect(t('connectPrompt.passwordLabel')).toBe('{host} requires a password')
    expect(t('scan.scanNumber', { unrelated: 'x' })).toBe('Scan #{id}')
  })

  it('returns the active locale text after setLocale', () => {
    setLocale('sr')
    expect(t('nav.devices')).toBe(sr.nav.devices)
  })
})

describe('setLocale', () => {
  beforeEach(() => {
    localStorage.clear()
    setLocale('en')
  })

  it('persists the choice and stamps document.documentElement.lang', () => {
    setLocale('sr')
    expect(localStorage.getItem('station-signal:locale')).toBe('sr')
    expect(document.documentElement.lang).toBe('sr')
  })

  it('drives the shared locale ref every consumer reads', () => {
    const { locale } = useI18n()
    setLocale('sr')
    expect(locale.value).toBe('sr')
    setLocale('en')
    expect(locale.value).toBe('en')
  })
})
