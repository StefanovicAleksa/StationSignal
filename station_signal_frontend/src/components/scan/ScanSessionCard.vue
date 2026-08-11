<script setup lang="ts">
import { computed } from 'vue'
import { Square } from '@lucide/vue'

import type { ScanSession } from '@/stores/scan'
import ScanStatusBanner from './ScanStatusBanner.vue'
import ScanResultsTable from './ScanResultsTable.vue'
import Panel from '@/components/ui/Panel.vue'
import Button from '@/components/ui/Button.vue'
import { useI18n } from '@/i18n'
import type { ConnectPreset } from '@/utils/connectPreset'

const props = defineProps<{
  session: ScanSession
}>()

const emit = defineEmits<{
  stop: []
  retryNow: []
  connect: [host: string, mmsPort: number, interfaceId: string, preset: ConnectPreset]
  view: [host: string, mmsPort: number]
}>()

const { t } = useI18n()

const isBusy = computed(() => props.session.phase === 'starting' || props.session.phase === 'stopping')

function handleConnect(host: string, mmsPort: number, preset: ConnectPreset) {
  emit('connect', host, mmsPort, props.session.interfaceId, preset)
}
</script>

<template>
  <Panel class="flex flex-col gap-3">
    <header class="flex items-center justify-between">
      <div>
        <h2 class="text-sm font-semibold text-slate-800 dark:text-slate-100">
          {{ session.interfaceId }}
          <span class="font-normal text-slate-400 dark:text-slate-500">
            {{ t('scan.portSummary', { port: session.mmsPort }) }}
          </span>
        </h2>
        <p v-if="session.scanId !== null" class="text-xs text-slate-400 dark:text-slate-500">
          {{ t('scan.scanNumber', { id: session.scanId }) }}
        </p>
      </div>
      <Button
        v-if="session.scanId !== null"
        variant="danger"
        size="sm"
        :icon="Square"
        :disabled="isBusy"
        @click="emit('stop')"
      >
        {{ t('common.stop') }}
      </Button>
    </header>

    <ScanStatusBanner
      :phase="session.phase"
      :error="session.error"
      :connection-interrupted="session.connectionInterrupted"
      :next-retry-at="session.nextRetryAt"
      @retry-now="emit('retryNow')"
    />

    <ScanResultsTable
      :results="session.results"
      @connect="handleConnect"
      @view="(host, mmsPort) => emit('view', host, mmsPort)"
    />
  </Panel>
</template>
