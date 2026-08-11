<script setup lang="ts">
import { computed } from 'vue'
import { Plug, Eye } from '@lucide/vue'

import type { ScanResultMessage } from '@/types/api'
import { useDevicesStore, type DevicePhase } from '@/stores/devices'
import { useDevicePhaseLabel } from '@/composables/useDevicePhaseLabel'
import { useI18n } from '@/i18n'
import { connectPresetForEvent, type ConnectPreset } from '@/utils/connectPreset'
import Table from '@/components/ui/Table.vue'
import Th from '@/components/ui/Th.vue'
import Td from '@/components/ui/Td.vue'
import Badge from '@/components/ui/Badge.vue'
import Button from '@/components/ui/Button.vue'
import ShortcutHint from '@/components/ui/ShortcutHint.vue'

const props = defineProps<{
  results: ScanResultMessage[]
}>()

const emit = defineEmits<{
  connect: [host: string, mmsPort: number, preset: ConnectPreset]
  view: [host: string, mmsPort: number]
}>()

const devicesStore = useDevicesStore()
const { t } = useI18n()
const { label: phaseLabel, tone: phaseTone } = useDevicePhaseLabel()

// A scan result's row state is derived from the devices store rather than tracked here, so
// stopping a device puts its Connect button back automatically. This replaced an approach that
// deleted the row outright on a successful connect: the daemon's scan worker keeps a per-scan
// seen-set and never republishes a host it has already reported, so a deleted row was gone for
// the life of that scan and disconnecting left no way back to the device.
const rows = computed(() =>
  [...props.results].reverse().map((result) => {
    const device = devicesStore.findByHost(result.host, result.mmsPort)
    // An errored device is not an active connection — the row offers Connect again so a failed
    // attempt can be retried in place, matching the devices store's own retry rule.
    const phase: DevicePhase | null = device && device.phase !== 'error' ? device.phase : null
    return { result, phase }
  }),
)

const shortcutItems = computed(() => [
  { key: 'Shift', text: t('shortcuts.connectDefault') },
  { key: 'Ctrl', text: t('shortcuts.connectAll') },
])

function formatTime(ms: number): string {
  return new Date(ms).toLocaleTimeString()
}
</script>

<template>
  <div class="flex flex-col gap-2">
    <Table>
      <thead class="bg-slate-50 dark:bg-slate-800/60">
        <tr>
          <Th>{{ t('fields.host') }}</Th>
          <Th>{{ t('fields.mmsPort') }}</Th>
          <Th>{{ t('scan.results.discovered') }}</Th>
          <Th></Th>
        </tr>
      </thead>
      <tbody class="divide-y divide-slate-100 bg-white dark:divide-slate-800 dark:bg-slate-900">
        <tr v-if="rows.length === 0">
          <td colspan="4" class="px-4 py-6 text-center text-slate-400 dark:text-slate-500">
            {{ t('scan.results.empty') }}
          </td>
        </tr>
        <tr
          v-for="{ result, phase } in rows"
          :key="`${result.host}-${result.mmsPort}-${result.discoveredAtMs}`"
        >
          <Td mono>{{ result.host }}</Td>
          <Td muted>{{ result.mmsPort }}</Td>
          <Td muted>{{ formatTime(result.discoveredAtMs) }}</Td>
          <Td align="right">
            <div class="flex items-center justify-end gap-2">
              <Badge v-if="phase" :tone="phaseTone[phase]">{{ phaseLabel[phase] }}</Badge>

              <!-- Mid-stop: neither connectable nor viewable, so the action is simply absent
                   until the device leaves the store and the row reverts to Connect. -->
              <Button
                v-if="phase && phase !== 'stopping'"
                variant="ghost"
                size="sm"
                :icon="Eye"
                @click="emit('view', result.host, result.mmsPort)"
              >
                {{ t('common.view') }}
              </Button>

              <Button
                v-else-if="!phase"
                variant="ghost"
                size="sm"
                :icon="Plug"
                :title="t('shortcuts.connectTooltip')"
                @click="(e: MouseEvent) => emit('connect', result.host, result.mmsPort, connectPresetForEvent(e))"
              >
                {{ t('common.connect') }}
              </Button>
            </div>
          </Td>
        </tr>
      </tbody>
    </Table>

    <ShortcutHint v-if="rows.length > 0" :items="shortcutItems" />
  </div>
</template>
