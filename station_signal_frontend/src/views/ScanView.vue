<script setup lang="ts">
import { onMounted, onUnmounted, computed, ref } from 'vue'
import { useRouter } from 'vue-router'

import { useScanStore } from '@/stores/scan'
import { useDevicesStore } from '@/stores/devices'
import ScanForm from '@/components/scan/ScanForm.vue'
import ScanSessionCard from '@/components/scan/ScanSessionCard.vue'
import ScanStatusBanner from '@/components/scan/ScanStatusBanner.vue'
import DeviceConnectPrompt from '@/components/device/DeviceConnectPrompt.vue'
import Panel from '@/components/ui/Panel.vue'
import { useI18n } from '@/i18n'
import type { ConnectPreset } from '@/utils/connectPreset'

const store = useScanStore()
const devicesStore = useDevicesStore()
const router = useRouter()
const { t } = useI18n()

const connectError = ref<string | null>(null)
const connectPrompt = ref<InstanceType<typeof DeviceConnectPrompt>>()

const sessions = computed(() => Object.values(store.sessions))

onMounted(() => {
  store.reconcileOnMount()
})

onUnmounted(() => {
  store.dispose()
})

function handleStart(interfaceId: string, mmsPort: number) {
  store.start(interfaceId, mmsPort)
}

function connectToDevice(host: string, mmsPort: number, interfaceId: string, preset: ConnectPreset) {
  connectError.value = null
  connectPrompt.value?.connect(host, mmsPort, interfaceId, undefined, undefined, undefined, preset)
}

// A scan row for a host that's already being watched offers View instead of Connect (see
// ScanResultsTable). The row itself stays put either way — it goes back to offering Connect the
// moment that device leaves the devices store.
function viewDevice(host: string, mmsPort: number) {
  const device = devicesStore.findByHost(host, mmsPort)
  if (device?.deviceId == null) return
  router.push({ name: 'reports', query: { device: device.deviceId } })
}

function handleConnected(key: string) {
  const device = devicesStore.devices[key]
  router.push({ name: 'reports', query: device?.deviceId != null ? { device: device.deviceId } : {} })
}

function handleConnectError(message: string) {
  connectError.value = message
}
</script>

<template>
  <div class="flex flex-col gap-6">
    <header>
      <h1 class="text-xl font-semibold text-slate-900 dark:text-slate-50">{{ t('scan.title') }}</h1>
      <p class="text-sm text-slate-500 dark:text-slate-400">{{ t('scan.subtitle') }}</p>
    </header>

    <Panel>
      <ScanForm @start="handleStart" />
    </Panel>

    <ScanStatusBanner
      v-if="store.mountError"
      phase="error"
      :error="store.mountError"
      :connection-interrupted="false"
      :next-retry-at="store.mountNextRetryAt"
      @retry-now="store.retryMountNow"
    />

    <p
      v-if="connectError"
      class="rounded-md border border-red-300 bg-red-50 px-3 py-2 text-sm text-red-800 dark:border-red-800 dark:bg-red-900/30 dark:text-red-300"
    >
      {{ connectError }}
    </p>

    <DeviceConnectPrompt ref="connectPrompt" @connected="handleConnected" @error="handleConnectError" />

    <p v-if="sessions.length === 0" class="text-sm text-slate-400 dark:text-slate-500">{{ t('scan.noScans') }}</p>

    <ScanSessionCard
      v-for="session in sessions"
      :key="session.key"
      :session="session"
      @stop="store.stop(session.key)"
      @retry-now="store.retryNow(session.key)"
      @connect="connectToDevice"
      @view="viewDevice"
    />
  </div>
</template>
