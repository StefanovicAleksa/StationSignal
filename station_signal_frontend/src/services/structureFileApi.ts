import { apiClient } from './apiClient'
import type { UploadStructureFileResponse } from '@/types/api'

export function uploadStructureFile(file: File): Promise<UploadStructureFileResponse> {
  const form = new FormData()
  form.append('file', file)
  return apiClient.postForm<UploadStructureFileResponse>('/structure-files', form)
}
