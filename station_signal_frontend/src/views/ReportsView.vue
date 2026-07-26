<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import { useDevicesStore } from '@/stores/devices'
import TabStrip, { type TabStripItem } from '@/components/common/TabStrip.vue'
import DeviceReportPanel from '@/components/device/DeviceReportPanel.vue'

const route = useRoute()
const router = useRouter()
const store = useDevicesStore()

const deviceList = computed(() => Object.values(store.devices))

const phaseLabel: Record<string, string> = {
  connecting: 'Connecting…',
  connected: 'Connected',
  interrupted: 'Interrupted',
  stopping: 'Stopping…',
  error: 'Error',
}

const tabs = computed<TabStripItem[]>(() =>
  deviceList.value.map((device) => ({
    id: device.key,
    label: `${device.host}:${device.mmsPort}`,
    meta: phaseLabel[device.phase],
  })),
)

function keyForRouteDeviceId(): string | null {
  const raw = route.query.device
  const id = typeof raw === 'string' ? Number(raw) : NaN
  if (!Number.isFinite(id)) return null
  return deviceList.value.find((d) => d.deviceId === id)?.key ?? null
}

const activeKey = ref<string | null>(keyForRouteDeviceId() ?? deviceList.value[deviceList.value.length - 1]?.key ?? null)

watch(deviceList, (list) => {
  if (activeKey.value && list.some((d) => d.key === activeKey.value)) return
  activeKey.value = list[list.length - 1]?.key ?? null
})

watch(
  () => route.query.device,
  () => {
    const resolved = keyForRouteDeviceId()
    if (resolved) activeKey.value = resolved
  },
)

function selectTab(key: string) {
  activeKey.value = key
  const device = store.devices[key]
  router.replace({ name: 'reports', query: device?.deviceId != null ? { device: device.deviceId } : {} })
}

function stopReporting(key: string) {
  store.stopDevice(key)
}

function clearReports(key: string) {
  store.clearReports(key)
}
</script>

<template>
  <div class="flex flex-col gap-4">
    <header>
      <h1 class="text-xl font-semibold text-slate-900 dark:text-slate-50">Reports</h1>
      <p class="text-sm text-slate-500 dark:text-slate-400">
        Live GOOSE/MMS reports for every device currently being watched.
      </p>
    </header>

    <div v-if="deviceList.length === 0" class="flex flex-col gap-3">
      <p class="text-sm text-slate-500 dark:text-slate-400">
        No devices being watched. Connect to one from the
        <RouterLink :to="{ name: 'devices' }" class="text-blue-600 underline hover:no-underline dark:text-blue-400">
          Devices
        </RouterLink>
        page, or start one from the
        <RouterLink :to="{ name: 'scan' }" class="text-blue-600 underline hover:no-underline dark:text-blue-400">
          Network Scan
        </RouterLink>
        page.
      </p>
    </div>

    <template v-else>
      <TabStrip :tabs="tabs" :active-id="activeKey" @select="selectTab" />

      <DeviceReportPanel
        v-for="device in deviceList"
        v-show="device.key === activeKey"
        :key="device.key"
        :device="device"
        @stop="stopReporting(device.key)"
        @clear="clearReports(device.key)"
      />
    </template>
  </div>
</template>
