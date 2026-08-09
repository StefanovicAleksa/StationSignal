<script setup lang="ts">
import { computed } from 'vue'
import { Plug } from '@lucide/vue'
import type { ScanResultMessage } from '@/types/api'
import Table from '@/components/ui/Table.vue'
import Th from '@/components/ui/Th.vue'
import Td from '@/components/ui/Td.vue'
import Button from '@/components/ui/Button.vue'

const props = defineProps<{
  results: ScanResultMessage[]
}>()

const emit = defineEmits<{
  connect: [host: string, mmsPort: number, bypassCategoryModal: boolean]
}>()

const newestFirst = computed(() => [...props.results].reverse())

function formatTime(ms: number): string {
  return new Date(ms).toLocaleTimeString()
}
</script>

<template>
  <Table>
    <thead class="bg-slate-50 dark:bg-slate-800/60">
      <tr>
        <Th>Host</Th>
        <Th>MMS Port</Th>
        <Th>Discovered</Th>
        <Th></Th>
      </tr>
    </thead>
    <tbody class="divide-y divide-slate-100 bg-white dark:divide-slate-800 dark:bg-slate-900">
      <tr v-if="results.length === 0">
        <td colspan="4" class="px-4 py-6 text-center text-slate-400 dark:text-slate-500">No hosts discovered yet.</td>
      </tr>
      <tr v-for="result in newestFirst" :key="`${result.host}-${result.mmsPort}-${result.discoveredAtMs}`">
        <Td mono>{{ result.host }}</Td>
        <Td muted>{{ result.mmsPort }}</Td>
        <Td muted>{{ formatTime(result.discoveredAtMs) }}</Td>
        <Td align="right">
          <Button
            variant="ghost"
            size="sm"
            :icon="Plug"
            @click="(e: MouseEvent) => emit('connect', result.host, result.mmsPort, e.shiftKey)"
          >
            Connect
          </Button>
        </Td>
      </tr>
    </tbody>
  </Table>
</template>
