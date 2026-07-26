import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'

vi.mock('@/services/deviceApi', () => ({
  startReporting: vi.fn(),
  stopReporting: vi.fn(),
  listDevices: vi.fn(),
}))

vi.mock('@/services/deviceSocket', () => ({
  createDeviceSocket: vi.fn(() => ({
    connect: vi.fn(),
    disconnect: vi.fn(),
  })),
}))

import { startReporting, stopReporting, listDevices } from '@/services/deviceApi'
import { createDeviceSocket } from '@/services/deviceSocket'
import { useDevicesStore } from '@/stores/devices'
import { ApiError, type DeviceStreamMessage } from '@/types/api'

describe('useDevicesStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('starts reporting on a new device and tracks it as connecting', async () => {
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()

    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    expect(store.devices[key]?.host).toBe('10.0.0.5')
    expect(store.devices[key]?.phase).toBe('connecting')
    expect(store.devices[key]?.deviceId).toBe(1)
    expect(createDeviceSocket).toHaveBeenCalledTimes(1)
  })

  it('adds an optimistic entry synchronously, before the REST call resolves', async () => {
    let resolveStart: ((value: { deviceId: number; wsPort: number }) => void) | undefined
    vi.mocked(startReporting).mockReturnValue(
      new Promise((resolve) => {
        resolveStart = resolve
      }),
    )
    const store = useDevicesStore()

    const promise = store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })
    const key = Object.keys(store.devices)[0]

    expect(key).toBeDefined()
    expect(store.devices[key!]?.phase).toBe('connecting')
    expect(store.devices[key!]?.deviceId).toBeNull()

    resolveStart?.({ deviceId: 1, wsPort: 9000 })
    await promise

    expect(store.devices[key!]?.deviceId).toBe(1)
  })

  it('returns the existing device key instead of re-starting a matching host+port', async () => {
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()
    const firstKey = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    expect(key).toBe(firstKey)
    expect(startReporting).toHaveBeenCalledTimes(1)
  })

  it('adopts the existing device on HOST_ALREADY_RUNNING', async () => {
    vi.mocked(startReporting).mockRejectedValue(
      new ApiError({ code: 'HOST_ALREADY_RUNNING', message: 'already running', stage: null, detail: null }, 409),
    )
    vi.mocked(listDevices).mockResolvedValue([{ deviceId: 7, host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0', wsPort: 9000 }])
    const store = useDevicesStore()

    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    expect(store.devices[key]?.deviceId).toBe(7)
    expect(store.devices[key]?.host).toBe('10.0.0.5')
  })

  it('appends an incoming data point as its own report and marks the device connected', async () => {
    let capturedOnMessage: ((message: DeviceStreamMessage) => void) | undefined
    vi.mocked(createDeviceSocket).mockImplementation((_id, handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()
    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    capturedOnMessage?.({
      schemaVersion: 1,
      type: 'MMS_REPORT',
      source: { rcbReference: 'LD0/RP1', buffered: true },
      hasTimestamp: true,
      timestampMs: 123,
      dataPoints: [
        { reference: 'LD0/CSWI1$ST$Pos$stVal', value: 1, quality: { validity: 'GOOD', detailFlags: 0 }, previousValue: 0, previousQuality: null, label: null, previousLabel: null },
      ],
    })

    expect(store.devices[key]?.phase).toBe('connected')
    expect(store.devices[key]?.reports).toHaveLength(1)
    expect(store.devices[key]?.reports[0]?.reportType).toBe('MMS_REPORT')
    expect(store.devices[key]?.reports[0]?.value).toBe(1)
    expect(store.devices[key]?.reports[0]?.valueChanged).toBe(true)
    expect(store.devices[key]?.lastMessageAtMs).toBe(123)
  })

  it('splits every data point from a single message into its own report', async () => {
    let capturedOnMessage: ((message: DeviceStreamMessage) => void) | undefined
    vi.mocked(createDeviceSocket).mockImplementation((_id, handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()
    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    capturedOnMessage?.({
      schemaVersion: 1,
      type: 'GOOSE',
      source: { goCbRef: 'LD0/LLN0$GO$gcbStatus' },
      hasTimestamp: true,
      timestampMs: 200,
      dataPoints: [
        { reference: 'LD0/CSWI1$ST$Pos$stVal', value: 1, quality: { validity: 'GOOD', detailFlags: 0 }, previousValue: 0, previousQuality: null, label: null, previousLabel: null },
        { reference: 'LD0/CSWI1$ST$Health$stVal', value: 1, quality: { validity: 'GOOD', detailFlags: 0 }, previousValue: 1, previousQuality: null, label: null, previousLabel: null },
      ],
    })

    expect(store.devices[key]?.reports).toHaveLength(2)
    expect(store.devices[key]?.reports[0]?.reference).toBe('LD0/CSWI1$ST$Pos$stVal')
    expect(store.devices[key]?.reports[1]?.reference).toBe('LD0/CSWI1$ST$Health$stVal')
  })

  it('flags a quality-only change independently of value, without flagging the value as changed', async () => {
    let capturedOnMessage: ((message: DeviceStreamMessage) => void) | undefined
    vi.mocked(createDeviceSocket).mockImplementation((_id, handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()
    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    capturedOnMessage?.({
      schemaVersion: 1,
      type: 'MMS_REPORT',
      source: { rcbReference: 'LD0/RP1', buffered: true },
      hasTimestamp: true,
      timestampMs: 300,
      dataPoints: [
        {
          reference: 'LD0/CSWI1$ST$Pos$stVal',
          value: 1,
          quality: { validity: 'QUESTIONABLE', detailFlags: 0 },
          previousValue: 1,
          previousQuality: { validity: 'GOOD', detailFlags: 0 },
          label: null,
          previousLabel: null,
        },
      ],
    })

    const report = store.devices[key]?.reports[0]
    expect(report?.valueChanged).toBe(false)
    expect(report?.qualityChanged).toBe(true)
    expect(report?.previousQuality?.validity).toBe('GOOD')
    expect(report?.quality?.validity).toBe('QUESTIONABLE')
  })

  it('does not flag a first-ever observation (no prior cache slot) as changed', async () => {
    let capturedOnMessage: ((message: DeviceStreamMessage) => void) | undefined
    vi.mocked(createDeviceSocket).mockImplementation((_id, handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()
    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    capturedOnMessage?.({
      schemaVersion: 1,
      type: 'GOOSE',
      source: { goCbRef: 'LD0/LLN0$GO$gcbStatus' },
      hasTimestamp: true,
      timestampMs: 400,
      dataPoints: [
        { reference: 'LD0/CSWI1$ST$Pos$stVal', value: 2, quality: null, previousValue: null, previousQuality: null, label: null, previousLabel: null },
      ],
    })

    const report = store.devices[key]?.reports[0]
    expect(report?.valueChanged).toBe(false)
    expect(report?.qualityChanged).toBe(false)
  })

  it('caps the retained report log at the most recent entries', async () => {
    let capturedOnMessage: ((message: DeviceStreamMessage) => void) | undefined
    vi.mocked(createDeviceSocket).mockImplementation((_id, handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()
    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    for (let i = 0; i < 505; i++) {
      capturedOnMessage?.({
        schemaVersion: 1,
        type: 'GOOSE',
        source: { goCbRef: 'LD0/LLN0$GO$gcbStatus' },
        hasTimestamp: true,
        timestampMs: i,
        dataPoints: [
          { reference: 'LD0/CSWI1$ST$Pos$stVal', value: i, quality: null, previousValue: i - 1, previousQuality: null, label: null, previousLabel: null },
        ],
      })
    }

    const reports = store.devices[key]?.reports ?? []
    expect(reports).toHaveLength(500)
    expect(reports[0]?.receivedAtMs).toBe(5)
    expect(reports[reports.length - 1]?.receivedAtMs).toBe(504)
  })

  it('AUTH_REQUIRED is re-thrown unchanged so the caller can prompt for a password', async () => {
    vi.mocked(startReporting).mockRejectedValue(
      new ApiError({ code: 'AUTH_REQUIRED', message: 'the device requires ACSE authentication', stage: null, detail: null }, 401),
    )
    const store = useDevicesStore()

    await expect(store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })).rejects.toMatchObject({
      code: 'AUTH_REQUIRED',
    })
  })

  it('a rejected connect leaves an errored row visible until retried or dismissed', async () => {
    vi.mocked(startReporting).mockRejectedValue(
      new ApiError({ code: 'AUTH_REQUIRED', message: 'the device requires ACSE authentication', stage: null, detail: null }, 401),
    )
    const store = useDevicesStore()

    await expect(store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })).rejects.toBeDefined()

    const key = Object.keys(store.devices)[0]
    expect(key).toBeDefined()
    expect(store.devices[key!]?.phase).toBe('error')
    expect(store.devices[key!]?.error?.code).toBe('AUTH_REQUIRED')
    expect(store.devices[key!]?.deviceId).toBeNull()
  })

  it('retrying startDevice with the correct acseAuthPassword reuses the same key and succeeds', async () => {
    vi.mocked(startReporting)
      .mockRejectedValueOnce(
        new ApiError({ code: 'AUTH_REQUIRED', message: 'the device requires ACSE authentication', stage: null, detail: null }, 401),
      )
      .mockResolvedValueOnce({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()

    await expect(store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })).rejects.toMatchObject({
      code: 'AUTH_REQUIRED',
    })
    const firstKey = Object.keys(store.devices)[0]

    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0', acseAuthPassword: 'secret123' })

    expect(key).toBe(firstKey)
    expect(store.devices[key]?.deviceId).toBe(1)
    expect(store.devices[key]?.phase).toBe('connecting')
    expect(Object.keys(store.devices)).toHaveLength(1)
    expect(startReporting).toHaveBeenNthCalledWith(2, {
      host: '10.0.0.5',
      mmsPort: 102,
      interfaceId: 'eth0',
      acseAuthPassword: 'secret123',
    })
  })

  it('a CONNECTION_STATUS/CONNECTION_REJECTED push marks an already-connected device as needing a password', async () => {
    let capturedOnMessage: ((message: DeviceStreamMessage) => void) | undefined
    vi.mocked(createDeviceSocket).mockImplementation((_id, handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()
    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })
    expect(store.devices[key]?.phase).toBe('connecting')

    capturedOnMessage?.({ schemaVersion: 1, type: 'CONNECTION_STATUS', status: 'CONNECTION_REJECTED' })

    expect(store.devices[key]?.phase).toBe('error')
    expect(store.devices[key]?.error?.code).toBe('AUTH_REQUIRED')
  })

  it('stopDevice tears down the socket and removes the device', async () => {
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    vi.mocked(stopReporting).mockResolvedValue({ deviceId: 1 })
    const store = useDevicesStore()
    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    await store.stopDevice(key)

    expect(store.devices[key]).toBeUndefined()
    expect(stopReporting).toHaveBeenCalledWith(1)
  })

  it('stopDevice on a still-pending (no real deviceId) entry skips the REST call', async () => {
    let resolveStart: ((value: { deviceId: number; wsPort: number }) => void) | undefined
    vi.mocked(startReporting).mockReturnValue(
      new Promise((resolve) => {
        resolveStart = resolve
      }),
    )
    const store = useDevicesStore()
    const startPromise = store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })
    const key = Object.keys(store.devices)[0]!

    await store.stopDevice(key)

    expect(store.devices[key]).toBeUndefined()
    expect(stopReporting).not.toHaveBeenCalled()

    resolveStart?.({ deviceId: 1, wsPort: 9000 })
    await startPromise
    expect(stopReporting).toHaveBeenCalledWith(1)
  })

  it('clearReports empties a device report log without stopping the watch', async () => {
    let capturedOnMessage: ((message: DeviceStreamMessage) => void) | undefined
    vi.mocked(createDeviceSocket).mockImplementation((_id, handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startReporting).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const store = useDevicesStore()
    const key = await store.startDevice({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })

    capturedOnMessage?.({
      schemaVersion: 1,
      type: 'GOOSE',
      source: { goCbRef: 'LD0/LLN0$GO$gcbStatus' },
      hasTimestamp: true,
      timestampMs: 1,
      dataPoints: [
        { reference: 'LD0/CSWI1$ST$Pos$stVal', value: 1, quality: null, previousValue: null, previousQuality: null, label: null, previousLabel: null },
      ],
    })
    expect(store.devices[key]?.reports).toHaveLength(1)

    store.clearReports(key)

    expect(store.devices[key]?.reports).toHaveLength(0)
    expect(store.devices[key]).toBeDefined()
  })

  it('reconcileOnLoad adds devices returned by the API that are not yet tracked', async () => {
    vi.mocked(listDevices).mockResolvedValue([{ deviceId: 3, host: '10.0.0.9', mmsPort: 102, interfaceId: 'eth1', wsPort: 9001 }])
    const store = useDevicesStore()

    await store.reconcileOnLoad()

    const key = Object.keys(store.devices)[0]!
    expect(store.devices[key]?.host).toBe('10.0.0.9')
    expect(store.devices[key]?.deviceId).toBe(3)
    expect(createDeviceSocket).toHaveBeenCalledTimes(1)
  })
})
