<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Play } from '@lucide/vue'
import Button from '@/components/ui/Button.vue'
import Disclosure from '@/components/ui/Disclosure.vue'
import { useSettingsStore } from '@/stores/settings'
import { useI18n } from '@/i18n'

const props = withDefaults(
  defineProps<{
    disabled?: boolean
  }>(),
  { disabled: false },
)

const emit = defineEmits<{
  start: [interfaceId: string, mmsPort: number]
}>()

// Defaults to the box's own active network interface (from the Settings page's network status)
// rather than a hardcoded name — a technician almost always wants to scan on the interface the
// box is actually configured on, and a fixed default here previously leaked a dev machine's own
// interface name into the shipped app.
const settingsStore = useSettingsStore()
const { t } = useI18n()

const interfaceId = ref('')
const mmsPortInput = ref('102')
const touched = ref(false)

onMounted(async () => {
  if (!settingsStore.status) {
    await settingsStore.loadStatus()
  }
  if (!interfaceId.value && settingsStore.status?.interface) {
    interfaceId.value = settingsStore.status.interface
  }
})

const interfaceError = computed(() => {
  if (!touched.value) return null
  return interfaceId.value.trim().length === 0 ? t('fields.interfaceRequired') : null
})

const portError = computed(() => {
  if (!touched.value) return null
  const value = Number(mmsPortInput.value)
  if (!Number.isInteger(value) || value < 1 || value > 65535) {
    return t('fields.portRange')
  }
  return null
})

const isValid = computed(() => {
  const value = Number(mmsPortInput.value)
  return (
    interfaceId.value.trim().length > 0 &&
    Number.isInteger(value) &&
    value >= 1 &&
    value <= 65535
  )
})

// The MMS port is a per-scan choice (an IED on a non-standard port is common enough), so it sits
// in the main row next to Start. Only the interface stays behind Advanced — it's prefilled from
// the box's own network status, with its current value shown inline. Force-opened whenever it's
// invalid, and whenever it never resolved (status not loaded), since a blank required field must
// not hide.
const advancedSummary = computed(() => interfaceId.value || '—')
const advancedForceOpen = computed(
  () => Boolean(interfaceError.value) || interfaceId.value.trim().length === 0,
)

function handleSubmit() {
  touched.value = true
  if (!isValid.value) return
  emit('start', interfaceId.value.trim(), Number(mmsPortInput.value))
  interfaceId.value = settingsStore.status?.interface ?? ''
  touched.value = false
}
</script>

<template>
  <form class="flex flex-col gap-3" @submit.prevent="handleSubmit">
    <!-- Error below the row, not inside the port column — see ManualConnectForm for why. -->
    <div class="flex flex-col gap-1">
      <div class="flex flex-wrap items-end gap-4">
        <div class="flex w-28 flex-col gap-1">
          <label for="mmsPort" class="text-sm font-medium text-slate-700 dark:text-slate-300">
            {{ t('fields.mmsPort') }}
          </label>
          <input
            id="mmsPort"
            v-model="mmsPortInput"
            type="number"
            min="1"
            max="65535"
            step="1"
            :disabled="props.disabled"
            class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
          />
        </div>

        <Button type="submit" variant="primary" :icon="Play" :disabled="props.disabled">
          {{ t('scan.startScan') }}
        </Button>
      </div>

      <p v-if="portError" class="text-xs text-red-600 dark:text-red-400">{{ portError }}</p>
    </div>

    <Disclosure :summary="advancedSummary" :force-open="advancedForceOpen">
      <div class="flex min-w-40 flex-1 flex-col gap-1">
        <label for="interfaceId" class="text-sm font-medium text-slate-700 dark:text-slate-300">
          {{ t('fields.interface') }}
        </label>
        <input
          id="interfaceId"
          v-model="interfaceId"
          type="text"
          :placeholder="t('fields.interfacePlaceholder')"
          :disabled="props.disabled"
          class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
        />
        <p v-if="interfaceError" class="text-xs text-red-600 dark:text-red-400">{{ interfaceError }}</p>
      </div>
    </Disclosure>
  </form>
</template>
