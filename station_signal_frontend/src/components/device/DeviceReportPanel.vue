<script setup lang="ts">
import { computed, ref } from 'vue'
import { Trash2, Square } from '@lucide/vue'

import type { DeviceReport, WatchedDevice } from '@/stores/devices'
import { formatApiErrorDetail, type LnCategory } from '@/types/api'
import { useDevicePhaseLabel } from '@/composables/useDevicePhaseLabel'
import { useI18n } from '@/i18n'
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

const { t } = useI18n()
const { detailedLabel: phaseLabel } = useDevicePhaseLabel()

// Per-browser view filter over an already-received stream — it never changes what the device is
// subscribed to. This is what lets two technicians share one physical connection to an IED (the
// API attaches every session at the same host:mmsPort to a single device) while each watches a
// different part of it. Empty set means no filtering.
const hiddenCategories = ref(new Set<LnCategory>())

const categoryOptions = computed<{ value: LnCategory; label: string }[]>(() => [
  { value: 'CONTROL', label: t('categoryModal.control') },
  { value: 'MEASUREMENT', label: t('categoryModal.measurement') },
  { value: 'PROTECTION', label: t('categoryModal.protection') },
  { value: 'OTHER', label: t('categoryModal.other') },
])

function toggleCategory(value: LnCategory) {
  const next = new Set(hiddenCategories.value)
  if (next.has(value)) next.delete(value)
  else next.add(value)
  hiddenCategories.value = next
}

function isCategoryShown(value: LnCategory): boolean {
  return !hiddenCategories.value.has(value)
}

// A point whose category the daemon couldn't resolve is always shown — hiding data purely
// because its category is unknown would silently drop real reports.
const reports = computed(() =>
  [...props.device.reports]
    .reverse()
    .filter((report) => report.category === null || !hiddenCategories.value.has(report.category)),
)

const hasHiddenReports = computed(() => hiddenCategories.value.size > 0 && props.device.reports.length > 0)

// Non-fatal: this session was attached to a device another session already started, so its own
// category choice was never applied (see the devices store's applyEffectiveCategories).
const sharedCategoriesLabel = computed(() => {
  const effective = props.device.effectiveCategories
  if (!effective || effective.length === 0) return t('reports.shared.allCategories')
  const labels = new Map(categoryOptions.value.map((option) => [option.value, option.label]))
  return effective.map((category) => labels.get(category) ?? category).join(', ')
})

const capabilityWarning = computed(() => {
  if (!props.device.mmsAvailable) return t('reports.capability.noMms')
  if (!props.device.gooseAvailable) return t('reports.capability.noGoose')
  return null
})

function formatValue(value: boolean | number | string | null): string {
  if (value === null) return t('common.empty')
  return String(value)
}

function formatTime(ms: number): string {
  return new Date(ms).toLocaleTimeString()
}

function sourceLabel(report: DeviceReport): string {
  if (report.reportType === 'MMS_REPORT') {
    const source = report.source as { rcbReference: string | null; buffered: boolean }
    const rcb = source.rcbReference ?? t('common.empty')
    return source.buffered ? t('reports.table.buffered', { rcb }) : rcb
  }
  const source = report.source as { goCbRef: string | null }
  return source.goCbRef ?? t('common.empty')
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
          {{ t('reports.interfaceAndPhase', { interfaceId: device.interfaceId, phase: phaseLabel[device.phase] }) }}
        </p>
      </div>
      <div class="flex shrink-0 items-center gap-2">
        <Button :disabled="device.reports.length === 0" @click="emit('clear')" :icon="Trash2">
          {{ t('common.clear') }}
        </Button>
        <Button variant="danger" :icon="Square" @click="emit('stop')">{{ t('reports.stopReporting') }}</Button>
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

    <!-- The API shares one physical connection per host:mmsPort across every browser session and
         keeps the creating session's own filter, so a second technician's category choice is
         silently dropped. Saying so beats letting them wonder why they're seeing categories they
         didn't ask for (or missing ones they did). -->
    <p
      v-if="device.sharedWithDifferentFilter"
      class="rounded-md border border-blue-300 bg-blue-50 px-3 py-2 text-sm text-blue-800 dark:border-blue-800 dark:bg-blue-900/30 dark:text-blue-300"
    >
      {{ t('reports.shared.banner', { categories: sharedCategoriesLabel }) }}
    </p>

    <div class="flex flex-wrap items-center gap-2">
      <span class="text-xs font-medium text-slate-500 dark:text-slate-400">{{ t('reports.filter.label') }}</span>
      <button
        v-for="option in categoryOptions"
        :key="option.value"
        type="button"
        class="rounded-full border px-2.5 py-0.5 text-xs font-medium transition-colors"
        :class="
          isCategoryShown(option.value)
            ? 'border-blue-300 bg-blue-50 text-blue-800 dark:border-blue-800 dark:bg-blue-900/30 dark:text-blue-300'
            : 'border-slate-300 bg-transparent text-slate-400 dark:border-slate-700 dark:text-slate-500'
        "
        :aria-pressed="isCategoryShown(option.value)"
        @click="toggleCategory(option.value)"
      >
        {{ option.label }}
      </button>
      <span class="text-xs text-slate-400 dark:text-slate-500">{{ t('reports.filter.hint') }}</span>
    </div>

    <Table>
      <thead class="bg-slate-50 dark:bg-slate-800/60">
        <tr>
          <Th>{{ t('reports.table.time') }}</Th>
          <Th>{{ t('reports.table.type') }}</Th>
          <Th>{{ t('reports.table.reference') }}</Th>
          <Th>{{ t('reports.table.value') }}</Th>
          <Th>{{ t('reports.table.quality') }}</Th>
        </tr>
      </thead>
      <tbody class="divide-y divide-slate-100 bg-white dark:divide-slate-800 dark:bg-slate-900">
        <tr v-if="reports.length === 0">
          <td colspan="5" class="px-4 py-6 text-center text-slate-400 dark:text-slate-500">
            <!-- Distinguishes "nothing has arrived" from "everything that arrived is filtered
                 out", which otherwise look identical and make the filter look broken. -->
            {{ hasHiddenReports ? t('reports.table.emptyForFilter') : t('reports.table.empty') }}
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
