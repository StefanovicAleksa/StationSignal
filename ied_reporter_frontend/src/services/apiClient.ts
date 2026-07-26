import { ApiError, type ApiErrorBody } from '@/types/api'

export const API_BASE_URL = import.meta.env.VITE_API_BASE_URL ?? 'http://localhost:8080'

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  // FormData bodies must not get an explicit Content-Type: fetch/the browser sets one
  // (multipart/form-data with the correct boundary) only when it's left unset.
  const isFormData = init?.body instanceof FormData
  const headers: HeadersInit = isFormData
    ? { ...init?.headers }
    : { 'Content-Type': 'application/json', ...init?.headers }

  const response = await fetch(`${API_BASE_URL}${path}`, { ...init, headers })

  if (!response.ok) {
    let body: ApiErrorBody
    try {
      const parsed = (await response.json()) as { error?: ApiErrorBody }
      body = parsed.error ?? { code: 'UNKNOWN', message: response.statusText, stage: null, detail: null }
    } catch {
      body = { code: 'UNKNOWN', message: response.statusText, stage: null, detail: null }
    }
    throw new ApiError(body, response.status)
  }

  if (response.status === 204) {
    return undefined as T
  }

  return (await response.json()) as T
}

export const apiClient = {
  get: <T>(path: string) => request<T>(path, { method: 'GET' }),
  post: <T>(path: string, body?: unknown) =>
    request<T>(path, { method: 'POST', body: body !== undefined ? JSON.stringify(body) : undefined }),
  postForm: <T>(path: string, form: FormData) => request<T>(path, { method: 'POST', body: form }),
  delete: <T>(path: string) => request<T>(path, { method: 'DELETE' }),
}
