import { apiClient } from './apiClient'
import type { DeviceSummary, StartDeviceRequest } from '@/types/api'

export function startReporting(request: StartDeviceRequest): Promise<{ deviceId: number; wsPort: number }> {
  return apiClient.post<{ deviceId: number; wsPort: number }>('/devices', request)
}

export function stopReporting(deviceId: number): Promise<{ deviceId: number }> {
  return apiClient.delete<{ deviceId: number }>(`/devices/${deviceId}`)
}

export function listDevices(): Promise<DeviceSummary[]> {
  return apiClient.get<DeviceSummary[]>('/devices')
}
