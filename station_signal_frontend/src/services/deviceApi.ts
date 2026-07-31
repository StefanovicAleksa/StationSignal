import { apiClient } from './apiClient'
import type { DeviceSummary, StartDeviceRequest, StartDeviceResponse } from '@/types/api'

export function startReporting(request: StartDeviceRequest): Promise<StartDeviceResponse> {
  return apiClient.post<StartDeviceResponse>('/devices', request)
}

export function stopReporting(deviceId: number): Promise<{ deviceId: number }> {
  return apiClient.delete<{ deviceId: number }>(`/devices/${deviceId}`)
}

// Recovery path for a device that never got (or has lost) a deviceId - see DELETE
// /api/devices?host=&mmsPort= in FRONTEND_API_GUIDE.md §2. Succeeds even if nothing was
// registered at this address (idempotent "already clean"); throws ApiError DEVICE_TRACKED if
// this API already has a record for it, or START_IN_PROGRESS if it's still mid-start.
export function stopReportingByAddress(host: string, mmsPort: number): Promise<{ host: string; mmsPort: number }> {
  const params = new URLSearchParams({ host, mmsPort: String(mmsPort) })
  return apiClient.delete<{ host: string; mmsPort: number }>(`/devices?${params.toString()}`)
}

export function listDevices(): Promise<DeviceSummary[]> {
  return apiClient.get<DeviceSummary[]>('/devices')
}
