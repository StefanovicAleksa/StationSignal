import { ref } from 'vue'
import { defineStore } from 'pinia'

import { startReporting, stopReporting, listDevices } from '@/services/deviceApi'
import { createDeviceSocket, type DeviceSocket } from '@/services/deviceSocket'
import type { DataPoint, DeviceStreamMessage, GooseSource, Quality, ReportSource, StartDeviceRequest } from '@/types/api'
import { ApiError } from '@/types/api'

export type DevicePhase = 'connecting' | 'connected' | 'interrupted' | 'stopping' | 'error'

export interface DeviceError {
  code: string
  message: string
  stage: string | null
  detail: string | null
}

export interface DeviceReport {
  id: number
  reportType: 'MMS_REPORT' | 'GOOSE'
  source: ReportSource | GooseSource
  receivedAtMs: number
  reference: string
  value: DataPoint['value']
  previousValue: DataPoint['value']
  valueChanged: boolean
  quality: Quality | null
  previousQuality: Quality | null
  qualityChanged: boolean
  label: string | null
  previousLabel: string | null
}

export interface WatchedDevice {
  key: string
  deviceId: number | null
  host: string
  mmsPort: number
  interfaceId: string
  phase: DevicePhase
  error: DeviceError | null
  reports: DeviceReport[]
  lastMessageAtMs: number | null
  // Which of MMS reporting / GOOSE subscription this device's SCL actually declares - a
  // device only needs one of the two, not both (see api.ts's own StartDeviceResponse doc).
  // Default true/true until a start/list response sets them, so nothing flashes a spurious
  // warning before the real value is known.
  mmsAvailable: boolean
  gooseAvailable: boolean
}

const MAX_REPORTS = 500

function hasValueChanged(point: DataPoint): boolean {
  return point.previousValue !== null && point.previousValue !== point.value
}

function hasQualityChanged(point: DataPoint): boolean {
  const current = point.quality
  const previous = point.previousQuality
  if (current === null && previous === null) return false
  if (current === null || previous === null) return true
  return current.validity !== previous.validity || current.detailFlags !== previous.detailFlags
}

export const useDevicesStore = defineStore('devices', () => {
  const devices = ref<Record<string, WatchedDevice>>({})

  const sockets = new Map<string, DeviceSocket>()
  let nextMessageId = 0
  let deviceSeq = 0

  function nextKey(): string {
    return `device-${deviceSeq++}`
  }

  function findByHost(host: string, mmsPort: number): WatchedDevice | undefined {
    return Object.values(devices.value).find((d) => d.host === host && d.mmsPort === mmsPort)
  }

  function connectDeviceSocket(key: string, deviceId: number) {
    const socket = createDeviceSocket(deviceId, {
      onMessage: (message: DeviceStreamMessage) => {
        const device = devices.value[key]
        if (!device) return

        if (message.type === 'CONNECTION_STATUS') {
          // Diagnostic-only, can arrive after this device already looked "connected" - the
          // report connection's own ACSE auth was rejected async (see ConnectionStatusMessage's
          // own doc comment in types/api.ts for the honest ambiguity here).
          device.phase = 'error'
          device.error = {
            code: 'AUTH_REQUIRED',
            message: 'Connection rejected — a password may be required',
            stage: null,
            detail: null,
          }
          return
        }

        const receivedAtMs = message.hasTimestamp && message.timestampMs ? message.timestampMs : Date.now()
        for (const point of message.dataPoints) {
          device.reports.push({
            id: nextMessageId++,
            reportType: message.type,
            source: message.source,
            receivedAtMs,
            reference: point.reference,
            value: point.value,
            previousValue: point.previousValue,
            valueChanged: hasValueChanged(point),
            quality: point.quality,
            previousQuality: point.previousQuality,
            qualityChanged: hasQualityChanged(point),
            label: point.label,
            previousLabel: point.previousLabel,
          })
        }
        if (device.reports.length > MAX_REPORTS) {
          device.reports.splice(0, device.reports.length - MAX_REPORTS)
        }
        device.lastMessageAtMs = receivedAtMs
        if (device.phase === 'interrupted' || device.phase === 'connecting') {
          device.phase = 'connected'
        }
      },
      onDisconnected: (reason) => {
        const device = devices.value[key]
        if (!device) return
        if (reason === 'dropped') {
          device.phase = 'interrupted'
        }
      },
    })
    sockets.set(key, socket)
    socket.connect()
  }

  async function startDevice(params: StartDeviceRequest): Promise<string> {
    const existing = findByHost(params.host, params.mmsPort ?? 102)
    if (existing && existing.phase !== 'error') {
      return existing.key
    }

    const key = existing ? existing.key : nextKey()
    devices.value[key] = {
      key,
      deviceId: existing?.deviceId ?? null,
      host: params.host,
      mmsPort: params.mmsPort ?? 102,
      interfaceId: params.interfaceId,
      phase: 'connecting',
      error: null,
      reports: existing?.reports ?? [],
      lastMessageAtMs: existing?.lastMessageAtMs ?? null,
      mmsAvailable: existing?.mmsAvailable ?? true,
      gooseAvailable: existing?.gooseAvailable ?? true,
    }

    try {
      const response = await startReporting(params)
      const current = devices.value[key]
      if (!current) {
        // stopped locally while the request was in flight — the daemon now has a device running
        // that we no longer track locally, so stop it to avoid leaking it.
        stopReporting(response.deviceId).catch(() => {})
        return key
      }
      current.deviceId = response.deviceId
      current.mmsAvailable = response.mmsAvailable
      current.gooseAvailable = response.gooseAvailable
      connectDeviceSocket(key, response.deviceId)
      return key
    } catch (err) {
      if (err instanceof ApiError && err.code === 'HOST_ALREADY_RUNNING') {
        const list = await listDevices()
        const match = list.find((d) => d.host === params.host && d.mmsPort === (params.mmsPort ?? 102))
        const current = devices.value[key]
        if (match && current) {
          current.deviceId = match.deviceId
          current.mmsAvailable = match.mmsAvailable
          current.gooseAvailable = match.gooseAvailable
          connectDeviceSocket(key, match.deviceId)
          return key
        }
        if (match) {
          return key
        }
      }
      const current = devices.value[key]
      if (current) {
        current.phase = 'error'
        current.error = toDeviceError(err)
      }
      throw err
    }
  }

  async function stopDevice(key: string) {
    const device = devices.value[key]
    if (!device) return

    if (device.deviceId === null) {
      // never got a real device id — still starting, or errored out (e.g. AUTH_REQUIRED) before
      // one was assigned. Nothing exists server-side to tear down; this also serves as the way
      // to dismiss a lingering errored row.
      sockets.get(key)?.disconnect()
      sockets.delete(key)
      delete devices.value[key]
      return
    }

    device.phase = 'stopping'
    try {
      await stopReporting(device.deviceId)
    } catch (err) {
      if (!(err instanceof ApiError && err.code === 'DEVICE_NOT_FOUND')) {
        device.phase = 'error'
        device.error = toDeviceError(err)
        return
      }
    }

    sockets.get(key)?.disconnect()
    sockets.delete(key)
    delete devices.value[key]
  }

  function clearReports(key: string) {
    const device = devices.value[key]
    if (!device) return
    device.reports = []
  }

  async function reconcileOnLoad() {
    try {
      const list = await listDevices()
      const known = new Set(
        Object.values(devices.value)
          .map((d) => d.deviceId)
          .filter((id): id is number => id !== null),
      )
      for (const summary of list) {
        if (known.has(summary.deviceId)) continue
        const key = nextKey()
        devices.value[key] = {
          key,
          deviceId: summary.deviceId,
          host: summary.host,
          mmsPort: summary.mmsPort,
          interfaceId: summary.interfaceId,
          phase: 'connecting',
          error: null,
          reports: [],
          lastMessageAtMs: null,
          mmsAvailable: summary.mmsAvailable,
          gooseAvailable: summary.gooseAvailable,
        }
        connectDeviceSocket(key, summary.deviceId)
      }
    } catch {
      // best-effort — the devices list/detail views will simply show nothing to reconnect to
    }
  }

  return {
    devices,
    startDevice,
    stopDevice,
    clearReports,
    reconcileOnLoad,
  }
})

function toDeviceError(err: unknown): DeviceError {
  if (err instanceof ApiError) {
    return { code: err.code, message: err.message, stage: err.stage, detail: err.detail }
  }
  return {
    code: 'UNKNOWN',
    message: err instanceof Error ? err.message : 'Unexpected error',
    stage: null,
    detail: null,
  }
}
