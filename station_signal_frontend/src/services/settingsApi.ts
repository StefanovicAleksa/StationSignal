import { apiClient } from './apiClient'
import type { NetworkConfig, NetworkPendingChange, NetworkStatus } from '@/types/api'

export function getNetworkStatus(): Promise<NetworkStatus> {
  return apiClient.get<NetworkStatus>('/settings/network')
}

export function applyNetworkConfig(config: NetworkConfig): Promise<NetworkPendingChange> {
  return apiClient.post<NetworkPendingChange>('/settings/network', config)
}

// isReachableAt/confirmNetworkConfigAt deliberately bypass apiClient (which always targets this
// page's own origin, via API_BASE_URL) — after submitting a new IP, the box we need to reach is
// no longer at this page's origin, it's at the address we just applied. These are used only by
// stores/settings.ts's post-apply reconnect flow.

export async function isReachableAt(baseUrl: string): Promise<boolean> {
  try {
    const res = await fetch(`${baseUrl}/health`)
    return res.ok
  } catch {
    return false
  }
}

export async function confirmNetworkConfigAt(baseUrl: string): Promise<void> {
  const res = await fetch(`${baseUrl}/settings/network/confirm`, { method: 'POST' })
  if (!res.ok) {
    throw new Error(`confirm failed with status ${res.status}`)
  }
}
