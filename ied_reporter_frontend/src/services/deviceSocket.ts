import { API_BASE_URL } from './apiClient'
import type { DeviceStreamMessage } from '@/types/api'

function wsUrl(deviceId: number): string {
  const url = new URL(`/ws/devices/${deviceId}`, API_BASE_URL)
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:'
  return url.toString()
}

export interface DeviceSocketHandlers {
  onMessage: (message: DeviceStreamMessage) => void
  onDisconnected: (reason: 'intentional' | 'dropped') => void
}

export interface DeviceSocket {
  connect: () => void
  disconnect: () => void
}

const RECONNECT_DELAY_MS = 750
const MAX_RECONNECT_ATTEMPTS = 5

export function createDeviceSocket(deviceId: number, handlers: DeviceSocketHandlers): DeviceSocket {
  let socket: WebSocket | null = null
  let intentionalClose = false
  let reconnectAttempts = 0
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null

  function open() {
    socket = new WebSocket(wsUrl(deviceId))

    socket.onmessage = (event) => {
      try {
        const message = JSON.parse(event.data) as DeviceStreamMessage
        if (message.type === 'MMS_REPORT' || message.type === 'GOOSE' || message.type === 'CONNECTION_STATUS') {
          handlers.onMessage(message)
        }
      } catch {
        // ignore malformed frames
      }
    }

    socket.onopen = () => {
      reconnectAttempts = 0
    }

    socket.onclose = () => {
      if (intentionalClose) {
        handlers.onDisconnected('intentional')
        return
      }

      handlers.onDisconnected('dropped')

      if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
        reconnectAttempts += 1
        reconnectTimer = setTimeout(open, RECONNECT_DELAY_MS)
      }
    }
  }

  return {
    connect() {
      intentionalClose = false
      reconnectAttempts = 0
      open()
    },
    disconnect() {
      intentionalClose = true
      if (reconnectTimer) {
        clearTimeout(reconnectTimer)
        reconnectTimer = null
      }
      socket?.close()
      socket = null
    },
  }
}
