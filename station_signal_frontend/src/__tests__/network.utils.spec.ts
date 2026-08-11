import { describe, it, expect } from 'vitest'

import { isValidIpv4, isValidPrefix, prefixOf, normalizeAddressInput, FALLBACK_PREFIX } from '@/utils/network'

describe('isValidIpv4', () => {
  it('accepts well-formed addresses', () => {
    for (const value of ['0.0.0.0', '172.16.0.50', '192.168.1.1', '255.255.255.255', '10.0.0.7']) {
      expect(isValidIpv4(value)).toBe(true)
    }
  })

  // The regex this replaced was /^(\d{1,3}\.){3}\d{1,3}$/, which accepted every one of these.
  it('rejects out-of-range octets', () => {
    for (const value of ['999.999.999.999', '256.0.0.1', '1.2.3.300']) {
      expect(isValidIpv4(value)).toBe(false)
    }
  })

  it('rejects malformed shapes', () => {
    for (const value of ['', '1.2.3', '1.2.3.4.5', '1.2.3.', 'a.b.c.d', '1.2.3.-1', '172.16.0.50/24']) {
      expect(isValidIpv4(value)).toBe(false)
    }
  })
})

describe('isValidPrefix', () => {
  it('accepts 1 through 30, matching the API', () => {
    expect(isValidPrefix(1)).toBe(true)
    expect(isValidPrefix(24)).toBe(true)
    expect(isValidPrefix(30)).toBe(true)
  })

  // station_signal_api's network/domain/config.go rejects /0 as too broad and /31-/32 as too
  // narrow for a usable host address; matching it here keeps the client from offering an address
  // the box will refuse.
  it('rejects prefixes the API refuses', () => {
    for (const prefix of [0, 31, 32, -1, 1.5]) {
      expect(isValidPrefix(prefix)).toBe(false)
    }
  })
})

describe('prefixOf', () => {
  it('extracts the prefix from a full CIDR', () => {
    expect(prefixOf('172.16.0.50/24')).toBe(24)
    expect(prefixOf('10.0.0.1/8')).toBe(8)
    expect(prefixOf('  192.168.1.50/30  ')).toBe(30)
  })

  it('returns null for a bare address or a malformed one', () => {
    expect(prefixOf('172.16.0.50')).toBeNull()
    expect(prefixOf('172.16.0.50/')).toBeNull()
    expect(prefixOf('172.16.0.50/24/8')).toBeNull()
    expect(prefixOf('172.16.0.50/32')).toBeNull()
    expect(prefixOf('999.1.1.1/24')).toBeNull()
  })
})

describe('normalizeAddressInput', () => {
  it('appends the fallback prefix to a bare address', () => {
    expect(normalizeAddressInput('172.16.0.50', 24)).toBe('172.16.0.50/24')
    expect(normalizeAddressInput('  172.16.0.50  ', 16)).toBe('172.16.0.50/16')
  })

  it('leaves an explicit prefix alone', () => {
    expect(normalizeAddressInput('172.16.0.50/16', 24)).toBe('172.16.0.50/16')
  })

  it('is idempotent, so a prefilled current address round-trips unchanged', () => {
    const once = normalizeAddressInput('172.16.0.50', FALLBACK_PREFIX)
    expect(once).not.toBeNull()
    expect(normalizeAddressInput(once as string, FALLBACK_PREFIX)).toBe(once)
  })

  it('returns null for invalid input', () => {
    expect(normalizeAddressInput('', 24)).toBeNull()
    expect(normalizeAddressInput('not-an-ip', 24)).toBeNull()
    expect(normalizeAddressInput('999.999.999.999', 24)).toBeNull()
    expect(normalizeAddressInput('172.16.0.50/33', 24)).toBeNull()
  })

  it('returns null rather than building an address the API would reject', () => {
    expect(normalizeAddressInput('172.16.0.50', 32)).toBeNull()
    expect(normalizeAddressInput('172.16.0.50', 0)).toBeNull()
  })
})
