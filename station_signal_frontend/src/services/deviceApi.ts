import { apiClient } from './apiClient'
import type { DeviceSummary, StartDeviceRequest, StartDeviceResponse } from '@/types/api'

export function startReporting(request: StartDeviceRequest): Promise<StartDeviceResponse> {
  return apiClient.post<StartDeviceResponse>('/devices', request)
}

export function stopReporting(deviceId: number): Promise<{ deviceId: number }> {
  return apiClient.delete<{ deviceId: number }>(`/devices/${deviceId}`)
}

export function listDevices(): Promise<DeviceSummary[]> {
  return apiClient.get<DeviceSummary[]>('/devices')
}
