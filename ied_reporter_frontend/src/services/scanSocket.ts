import { API_BASE_URL } from './apiClient'
import type { ScanResultMessage } from '@/types/api'

function wsUrl(): string {
  const url = new URL('/ws/scans', API_BASE_URL)
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:'
  return url.toString()
}

export interface ScanSocketHandlers {
  onMessage: (message: ScanResultMessage) => void
  onDisconnected: (reason: 'intentional' | 'dropped' | 'exhausted') => void
}

export interface ScanSocket {
  connect: () => void
  disconnect: () => void
}

const RECONNECT_DELAY_MS = 750
const MAX_RECONNECT_ATTEMPTS = 5

export function createScanSocket(handlers: ScanSocketHandlers): ScanSocket {
  let socket: WebSocket | null = null
  let intentionalClose = false
  let reconnectAttempts = 0
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null

  function open() {
    socket = new WebSocket(wsUrl())

    socket.onmessage = (event) => {
      try {
        const message = JSON.parse(event.data) as ScanResultMessage
        if (message.type === 'SCAN_RESULT') {
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

      if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
        handlers.onDisconnected('dropped')
        reconnectAttempts += 1
        reconnectTimer = setTimeout(open, RECONNECT_DELAY_MS)
      } else {
        handlers.onDisconnected('exhausted')
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
