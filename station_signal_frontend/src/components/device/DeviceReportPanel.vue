<script setup lang="ts">
import { computed } from 'vue'
import { Trash2, Square } from '@lucide/vue'

import type { DeviceReport, WatchedDevice } from '@/stores/devices'
import { formatApiErrorDetail } from '@/types/api'
import QualityBadge from '@/components/device/QualityBadge.vue'
import Button from '@/components/ui/Button.vue'
import Table from '@/components/ui/Table.vue'
import Th from '@/components/ui/Th.vue'
import Td from '@/components/ui/Td.vue'

const props = defineProps<{
  device: WatchedDevice
}>()

const emit = defineEmits<{
  stop: []
  clear: []
}>()

const reports = computed(() => [...props.device.reports].reverse())

const capabilityWarning = computed(() => {
  if (!props.device.mmsAvailable) {
    return 'This device has no MMS report control blocks in its SCL — only GOOSE will be shown here.'
  }
  if (!props.device.gooseAvailable) {
    return 'This device has no GOOSE control blocks in its SCL — only MMS reports will be shown here.'
  }
  return null
})

const phaseLabel: Record<string, string> = {
  connecting: 'Connecting…',
  connected: 'Connected',
  interrupted: 'Interrupted — retrying…',
  stopping: 'Stopping…',
  error: 'Error',
}

function formatValue(value: boolean | number | string | null): string {
  if (value === null) return '—'
  return String(value)
}

function formatTime(ms: number): string {
  return new Date(ms).toLocaleTimeString()
}

function sourceLabel(report: DeviceReport): string {
  if (report.reportType === 'MMS_REPORT') {
    const source = report.source as { rcbReference: string | null; buffered: boolean }
    const rcb = source.rcbReference ?? '—'
    return source.buffered ? `${rcb} (buffered)` : rcb
  }
  const source = report.source as { goCbRef: string | null }
  return source.goCbRef ?? '—'
}

function displayValue(value: string | boolean | number | null): string {
  return typeof value === 'string' ? value : formatValue(value)
}
</script>

<template>
  <div class="flex flex-col gap-4">
    <header class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
      <div>
        <h1 class="text-xl font-semibold text-slate-900 dark:text-slate-50">
          {{ device.host }}:{{ device.mmsPort }}
        </h1>
        <p class="text-sm text-slate-500 dark:text-slate-400">
          Interface {{ device.interfaceId }} — {{ phaseLabel[device.phase] }}
        </p>
      </div>
      <div class="flex shrink-0 items-center gap-2">
        <Button :disabled="device.reports.length === 0" @click="emit('clear')" :icon="Trash2">Clear</Button>
        <Button variant="danger" :icon="Square" @click="emit('stop')">Stop Reporting</Button>
      </div>
    </header>

    <p
      v-if="device.error"
      class="rounded-md border border-red-300 bg-red-50 px-3 py-2 text-sm text-red-800 dark:border-red-800 dark:bg-red-900/30 dark:text-red-300"
    >
      {{ formatApiErrorDetail(device.error) }}
    </p>

    <p
      v-if="capabilityWarning"
      class="rounded-md border border-amber-300 bg-amber-50 px-3 py-2 text-sm text-amber-800 dark:border-amber-800 dark:bg-amber-900/30 dark:text-amber-300"
    >
      {{ capabilityWarning }}
    </p>

    <Table>
      <thead class="bg-slate-50 dark:bg-slate-800/60">
        <tr>
          <Th>Time</Th>
          <Th>Type</Th>
          <Th>Reference</Th>
          <Th>Value</Th>
          <Th>Quality</Th>
        </tr>
      </thead>
      <tbody class="divide-y divide-slate-100 bg-white dark:divide-slate-800 dark:bg-slate-900">
        <tr v-if="reports.length === 0">
          <td colspan="5" class="px-4 py-6 text-center text-slate-400 dark:text-slate-500">
            No reports received yet.
          </td>
        </tr>
        <tr v-for="report in reports" :key="report.id" class="align-top">
          <Td muted nowrap>{{ formatTime(report.receivedAtMs) }}</Td>
          <Td muted>{{ report.reportType === 'MMS_REPORT' ? 'MMS' : 'GOOSE' }}</Td>
          <Td>
            <div class="flex flex-col gap-0.5">
              <span class="font-mono text-xs text-slate-800 dark:text-slate-200">{{ report.reference }}</span>
              <span class="font-mono text-xs text-slate-400 dark:text-slate-500">{{ sourceLabel(report) }}</span>
            </div>
          </Td>
          <Td>
            <span
              v-if="report.valueChanged"
              class="rounded bg-amber-100 px-1.5 py-0.5 text-xs font-medium text-amber-800 dark:bg-amber-900/40 dark:text-amber-300"
            >
              {{ displayValue(report.previousLabel ?? report.previousValue) }} →
              {{ displayValue(report.label ?? report.value) }}
            </span>
            <span v-else class="text-slate-800 dark:text-slate-200">{{ displayValue(report.label ?? report.value) }}</span>
          </Td>
          <Td>
            <span v-if="report.qualityChanged" class="flex items-center gap-1">
              <QualityBadge :validity="report.previousQuality?.validity ?? null" />
              <span class="text-slate-400 dark:text-slate-500">→</span>
              <QualityBadge :validity="report.quality?.validity ?? null" />
            </span>
            <QualityBadge v-else :validity="report.quality?.validity ?? null" />
          </Td>
        </tr>
      </tbody>
    </Table>
  </div>
</template>
