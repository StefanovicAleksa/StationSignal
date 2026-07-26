export type ErrorCode =
  | 'INVALID_ARGUMENT'
  | 'AUTH_REQUIRED'
  | 'DEVICE_NOT_FOUND'
  | 'SCAN_NOT_FOUND'
  | 'HOST_ALREADY_RUNNING'
  | 'START_IN_PROGRESS'
  | 'ORCHESTRATION_FAILED'
  | 'OUT_OF_MEMORY'
  | 'PORT_EXHAUSTED'
  | 'DISPATCHER_START_FAILED'
  | 'THREAD_CREATE_FAILED'
  | 'DISCOVERY_CREATE_FAILED'
  | 'DAEMON_UNREACHABLE'
  | string

export interface ApiErrorBody {
  code: ErrorCode
  message: string
  stage: string | null
  detail: string | null
}

export class ApiError extends Error {
  code: ErrorCode
  stage: string | null
  detail: string | null
  httpStatus: number

  constructor(body: ApiErrorBody, httpStatus: number) {
    super(body.message)
    this.name = 'ApiError'
    this.code = body.code
    this.stage = body.stage
    this.detail = body.detail
    this.httpStatus = httpStatus
  }
}

export function formatApiErrorDetail(err: { code: string; message: string; stage?: string | null; detail?: string | null }): string {
  const parts = [err.message, err.stage, err.detail].filter((part): part is string => Boolean(part))
  return `${parts.join(' — ')} (${err.code})`
}

// True for the API's synthesized AUTH_REQUIRED code (see FRONTEND_API_GUIDE.md §5) - a
// POST /devices call was rejected because the device needs acseAuthPassword (missing or wrong).
export function isAuthRequiredError(err: unknown): err is ApiError {
  return err instanceof ApiError && err.code === 'AUTH_REQUIRED'
}

export interface StartScanRequest {
  interfaceId: string
  mmsPort?: number
  sweepIntervalMs?: number
}

export interface ScanSummary {
  scanId: number
  interfaceId: string
  mmsPort: number
  sweepIntervalMs: number
}

export interface ScanResultMessage {
  schemaVersion: number
  type: 'SCAN_RESULT'
  scanId: number
  host: string
  mmsPort: number
  discoveredAtMs: number
}

export type AccessMode = 'REPORT_ONLY' | 'READ_ONLY' | 'READ_AND_WRITE'

export interface StartDeviceRequest {
  host: string
  mmsPort?: number
  iedName?: string
  interfaceId: string
  sclFilePath?: string
  acseAuthPassword?: string
  accessMode?: AccessMode
}

export interface UploadStructureFileResponse {
  path: string
}

export interface DeviceSummary {
  deviceId: number
  host: string
  mmsPort: number
  interfaceId: string
  wsPort: number
}

export interface Quality {
  validity: 'GOOD' | 'INVALID' | 'RESERVED' | 'QUESTIONABLE'
  detailFlags: number
}

export interface DataPoint {
  reference: string
  value: boolean | number | string | null
  quality: Quality | null
  previousValue: boolean | number | string | null
  previousQuality: Quality | null
  label: string | null
  previousLabel: string | null
}

export interface ReportSource {
  rcbReference: string | null
  buffered: boolean
}

export interface GooseSource {
  goCbRef: string | null
}

interface DeviceStreamMessageBase {
  schemaVersion: number
  hasTimestamp: boolean
  timestampMs?: number
  dataPoints: DataPoint[]
}

export interface MmsReportMessage extends DeviceStreamMessageBase {
  type: 'MMS_REPORT'
  source: ReportSource
}

export interface GooseMessage extends DeviceStreamMessageBase {
  type: 'GOOSE'
  source: GooseSource
}

// Diagnostic-only, not report/GOOSE data - can arrive after a device's POST /devices already
// returned success (the MMS report connection retries in the background). "Rejected" reuses
// the underlying protocol library's one "connection didn't succeed" code, so this isn't
// exclusively a wrong-password signal, even though that's the most common real cause - see
// FRONTEND_API_GUIDE.md §4's own caveat.
export interface ConnectionStatusMessage {
  schemaVersion: number
  type: 'CONNECTION_STATUS'
  status: 'CONNECTION_REJECTED'
}

export type DeviceStreamMessage = MmsReportMessage | GooseMessage | ConnectionStatusMessage
