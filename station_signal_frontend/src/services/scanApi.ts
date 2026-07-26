import { apiClient } from './apiClient'
import type { ScanSummary, StartScanRequest } from '@/types/api'

export function startScan(request: StartScanRequest): Promise<{ scanId: number }> {
  return apiClient.post<{ scanId: number }>('/scans', request)
}

export function stopScan(scanId: number): Promise<{ scanId: number }> {
  return apiClient.delete<{ scanId: number }>(`/scans/${scanId}`)
}

export function listScans(): Promise<ScanSummary[]> {
  return apiClient.get<ScanSummary[]>('/scans')
}
