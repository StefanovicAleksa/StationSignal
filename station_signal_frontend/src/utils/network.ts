// IPv4 / CIDR helpers for the network settings form.
//
// Extracted out of SettingsView.vue, which carried these as inline regexes with no test
// coverage. Two behavioral changes came with the move:
//   - octets are range-checked, so `999.999.999.999` is no longer accepted client-side;
//   - the prefix is optional on input, appended by normalizeAddressInput.
//
// The API itself still requires a full CIDR (station_signal_api's network/domain/config.go
// rejects a bare address as "missing prefix length"), so the convenience is deliberately a
// browser-side affordance — what gets submitted is always a complete CIDR.

// Matches the API's own accepted range (config.go rejects /0 as too broad and /31-/32 as too
// narrow for a usable host address).
export const MIN_PREFIX = 1
export const MAX_PREFIX = 30

// Used when the box's own current configuration can't supply one (status not loaded yet, or an
// unparseable current address).
export const FALLBACK_PREFIX = 24

export function isValidIpv4(value: string): boolean {
  const octets = value.split('.')
  if (octets.length !== 4) return false
  return octets.every((octet) => /^\d{1,3}$/.test(octet) && Number(octet) <= 255)
}

export function isValidPrefix(prefix: number): boolean {
  return Number.isInteger(prefix) && prefix >= MIN_PREFIX && prefix <= MAX_PREFIX
}

/** The prefix length of a full CIDR string, or null if it has none or is malformed. */
export function prefixOf(cidr: string): number | null {
  const parts = cidr.trim().split('/')
  if (parts.length !== 2) return null
  const [address, prefix] = parts as [string, string]
  if (!isValidIpv4(address) || !/^\d{1,2}$/.test(prefix)) return null
  const parsed = Number(prefix)
  return isValidPrefix(parsed) ? parsed : null
}

/**
 * Turns whatever the technician typed into a full CIDR, or null if it isn't a usable address.
 *
 * A bare `172.16.0.50` gets `/${fallbackPrefix}` appended; an explicit `172.16.0.50/16` passes
 * through unchanged. Idempotent on an already-complete CIDR, so it's safe to run over a value
 * that was prefilled from the box's current configuration.
 */
export function normalizeAddressInput(input: string, fallbackPrefix: number): string | null {
  const trimmed = input.trim()
  if (trimmed.length === 0) return null

  if (!trimmed.includes('/')) {
    return isValidIpv4(trimmed) && isValidPrefix(fallbackPrefix) ? `${trimmed}/${fallbackPrefix}` : null
  }
  return prefixOf(trimmed) === null ? null : trimmed
}
