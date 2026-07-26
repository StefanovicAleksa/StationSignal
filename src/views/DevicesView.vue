<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'
import { Eye, Square } from '@lucide/vue'

import { useDevicesStore } from '@/stores/devices'
import DeviceConnectPrompt from '@/components/device/DeviceConnectPrompt.vue'
import ManualConnectForm from '@/components/device/ManualConnectForm.vue'
import Panel from '@/components/ui/Panel.vue'
import Badge from '@/components/ui/Badge.vue'
import Button from '@/components/ui/Button.vue'
import Table from '@/components/ui/Table.vue'
import Th from '@/components/ui/Th.vue'
import Td from '@/components/ui/Td.vue'

const store = useDevicesStore()
const router = useRouter()

const connectError = ref<string | null>(null)
const connectPrompt = ref<InstanceType<typeof DeviceConnectPrompt>>()

function handleConnect(
  host: string,
  mmsPort: number,
  interfaceId: string,
  iedName?: string,
  sclFilePath?: string,
) {
  connectError.value = null
  connectPrompt.value?.connect(host, mmsPort, interfaceId, undefined, iedName, sclFilePath)
}

function handleConnected(key: string) {
  const device = store.devices[key]
  router.push({ name: 'reports', query: device?.deviceId != null ? { device: device.deviceId } : {} })
}

function handleConnectError(message: string) {
  connectError.value = message
}

const devices = computed(() => Object.values(store.devices).sort((a, b) => a.host.localeCompare(b.host)))

const phaseLabel: Record<string, string> = {
  connecting: 'Connecting…',
  connected: 'Connected',
  interrupted: 'Interrupted',
  stopping: 'Stopping…',
  error: 'Error',
}

const phaseTone: Record<string, 'success' | 'error' | 'warning' | 'neutral'> = {
  connecting: 'neutral',
  connected: 'success',
  interrupted: 'warning',
  stopping: 'neutral',
  error: 'error',
}

function formatTime(ms: number | null): string {
  return ms ? new Date(ms).toLocaleTimeString() : '—'
}
</script>

<template>
  <div class="flex flex-col gap-6">
    <header>
      <h1 class="text-xl font-semibold text-slate-900 dark:text-slate-50">Watched Devices</h1>
      <p class="text-sm text-slate-500 dark:text-slate-400">Devices currently being reported on, updated live.</p>
    </header>

    <Panel>
      <ManualConnectForm @connect="handleConnect" />
    </Panel>

    <p
      v-if="connectError"
      class="rounded-md border border-red-300 bg-red-50 px-3 py-2 text-sm text-red-800 dark:border-red-800 dark:bg-red-900/30 dark:text-red-300"
    >
      {{ connectError }}
    </p>

    <DeviceConnectPrompt ref="connectPrompt" @connected="handleConnected" @error="handleConnectError" />

    <Table>
      <thead class="bg-slate-50 dark:bg-slate-800/60">
        <tr>
          <Th>Host</Th>
          <Th>MMS Port</Th>
          <Th>Interface</Th>
          <Th>Status</Th>
          <Th>Last Message</Th>
          <Th></Th>
        </tr>
      </thead>
      <tbody class="divide-y divide-slate-100 bg-white dark:divide-slate-800 dark:bg-slate-900">
        <tr v-if="devices.length === 0">
          <td colspan="6" class="px-4 py-6 text-center text-slate-400 dark:text-slate-500">
            No devices being watched. Connect to one above, or start one from the
            <RouterLink :to="{ name: 'scan' }" class="text-blue-600 underline hover:no-underline dark:text-blue-400">
              Network Scan
            </RouterLink>
            page.
          </td>
        </tr>
        <tr v-for="device in devices" :key="device.key">
          <Td mono>{{ device.host }}</Td>
          <Td muted>{{ device.mmsPort }}</Td>
          <Td muted>{{ device.interfaceId }}</Td>
          <Td>
            <Badge :tone="phaseTone[device.phase]">{{ phaseLabel[device.phase] }}</Badge>
          </Td>
          <Td muted>{{ formatTime(device.lastMessageAtMs) }}</Td>
          <Td align="right">
            <div class="flex items-center justify-end gap-2">
              <RouterLink v-if="device.deviceId !== null" :to="{ name: 'reports', query: { device: device.deviceId } }">
                <Button variant="ghost" size="sm" :icon="Eye">View</Button>
              </RouterLink>
              <Button variant="danger" size="sm" :icon="Square" @click="store.stopDevice(device.key)">Stop</Button>
            </div>
          </Td>
        </tr>
      </tbody>
    </Table>
  </div>
</template>
