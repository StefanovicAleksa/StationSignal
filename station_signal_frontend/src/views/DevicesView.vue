<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'
import { Eye, Square } from '@lucide/vue'

import { useDevicesStore } from '@/stores/devices'
import { useDevicePhaseLabel } from '@/composables/useDevicePhaseLabel'
import { useI18n } from '@/i18n'
import type { ConnectPreset } from '@/utils/connectPreset'
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
const { t } = useI18n()
const { label: phaseLabel, tone: phaseTone } = useDevicePhaseLabel()

const connectError = ref<string | null>(null)
const connectPrompt = ref<InstanceType<typeof DeviceConnectPrompt>>()

function handleConnect(
  host: string,
  mmsPort: number,
  interfaceId: string,
  iedName?: string,
  sclFilePath?: string,
  preset: ConnectPreset = 'ask',
) {
  connectError.value = null
  connectPrompt.value?.connect(host, mmsPort, interfaceId, undefined, iedName, sclFilePath, preset)
}

function handleConnected(key: string) {
  const device = store.devices[key]
  router.push({ name: 'reports', query: device?.deviceId != null ? { device: device.deviceId } : {} })
}

function handleConnectError(message: string) {
  connectError.value = message
}

const devices = computed(() => Object.values(store.devices).sort((a, b) => a.host.localeCompare(b.host)))

function formatTime(ms: number | null): string {
  return ms ? new Date(ms).toLocaleTimeString() : t('common.empty')
}
</script>

<template>
  <div class="flex flex-col gap-6">
    <header>
      <h1 class="text-xl font-semibold text-slate-900 dark:text-slate-50">{{ t('devices.title') }}</h1>
      <p class="text-sm text-slate-500 dark:text-slate-400">{{ t('devices.subtitle') }}</p>
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
          <Th>{{ t('fields.host') }}</Th>
          <Th>{{ t('fields.mmsPort') }}</Th>
          <Th>{{ t('fields.interface') }}</Th>
          <Th>{{ t('devices.status') }}</Th>
          <Th>{{ t('devices.lastMessage') }}</Th>
          <Th></Th>
        </tr>
      </thead>
      <tbody class="divide-y divide-slate-100 bg-white dark:divide-slate-800 dark:bg-slate-900">
        <tr v-if="devices.length === 0">
          <td colspan="6" class="px-4 py-6 text-center text-slate-400 dark:text-slate-500">
            {{ t('devices.emptyBefore') }}
            <RouterLink :to="{ name: 'scan' }" class="text-blue-600 underline hover:no-underline dark:text-blue-400">
              {{ t('devices.emptyLink') }}
            </RouterLink>
            {{ t('devices.emptyAfter') }}
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
                <Button variant="ghost" size="sm" :icon="Eye">{{ t('common.view') }}</Button>
              </RouterLink>
              <Button variant="danger" size="sm" :icon="Square" @click="store.stopDevice(device.key)">
                {{ t('common.stop') }}
              </Button>
            </div>
          </Td>
        </tr>
      </tbody>
    </Table>
  </div>
</template>
