<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Plug } from '@lucide/vue'

import StructureFileUpload from '@/components/device/StructureFileUpload.vue'
import Button from '@/components/ui/Button.vue'
import { useSettingsStore } from '@/stores/settings'

const props = withDefaults(
  defineProps<{
    disabled?: boolean
  }>(),
  { disabled: false },
)

const emit = defineEmits<{
  connect: [
    host: string,
    mmsPort: number,
    interfaceId: string,
    iedName: string | undefined,
    sclFilePath: string | undefined,
    bypassCategoryModal: boolean,
  ]
}>()

// Defaults to the box's own active network interface (from the Settings page's network status)
// rather than a hardcoded name — see ScanForm.vue's identical rationale.
const settingsStore = useSettingsStore()

const host = ref('')
const mmsPortInput = ref('102')
const interfaceId = ref('')
const iedName = ref('')
const sclFilePath = ref('')
const touched = ref(false)

// Captured from the Connect button's own click (a form 'submit' event carries no modifier-key
// state) - click fires before submit, so this is always up to date by the time handleSubmit runs.
// Holding Shift while clicking Connect skips the category picker and connects unfiltered.
const lastClickShiftHeld = ref(false)

function captureShiftKey(event: MouseEvent) {
  lastClickShiftHeld.value = event.shiftKey
}

onMounted(async () => {
  if (!settingsStore.status) {
    await settingsStore.loadStatus()
  }
  if (!interfaceId.value && settingsStore.status?.interface) {
    interfaceId.value = settingsStore.status.interface
  }
})

const hostError = computed(() => {
  if (!touched.value) return null
  return host.value.trim().length === 0 ? 'Host is required.' : null
})

const interfaceError = computed(() => {
  if (!touched.value) return null
  return interfaceId.value.trim().length === 0 ? 'Interface is required.' : null
})

const portError = computed(() => {
  if (!touched.value) return null
  const value = Number(mmsPortInput.value)
  if (!Number.isInteger(value) || value < 1 || value > 65535) {
    return 'Port must be an integer between 1 and 65535.'
  }
  return null
})

// Mirrors the daemon's own rule: a structure file is only meaningful together with the IED
// name it defines, so require both or neither (see AGENT_API_GUIDE.md's START_REPORTING params).
const iedNameError = computed(() => {
  if (!touched.value) return null
  return sclFilePath.value.trim().length > 0 && iedName.value.trim().length === 0
    ? 'IED name is required when a structure file path is given.'
    : null
})

const isValid = computed(() => {
  const value = Number(mmsPortInput.value)
  return (
    host.value.trim().length > 0 &&
    interfaceId.value.trim().length > 0 &&
    Number.isInteger(value) &&
    value >= 1 &&
    value <= 65535 &&
    !(sclFilePath.value.trim().length > 0 && iedName.value.trim().length === 0)
  )
})

function handleSubmit() {
  touched.value = true
  if (!isValid.value) return
  emit(
    'connect',
    host.value.trim(),
    Number(mmsPortInput.value),
    interfaceId.value.trim(),
    iedName.value.trim() || undefined,
    sclFilePath.value.trim() || undefined,
    lastClickShiftHeld.value,
  )
}

function handleStructureFilePath(path: string | undefined) {
  sclFilePath.value = path ?? ''
}
</script>

<template>
  <form class="flex flex-wrap items-end gap-4" @submit.prevent="handleSubmit">
    <div class="flex min-w-40 flex-1 flex-col gap-1">
      <label for="manualHost" class="text-sm font-medium text-slate-700 dark:text-slate-300">Host</label>
      <input
        id="manualHost"
        v-model="host"
        type="text"
        placeholder="10.250.99.14"
        :disabled="props.disabled"
        class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
      <p v-if="hostError" class="text-xs text-red-600 dark:text-red-400">{{ hostError }}</p>
    </div>

    <div class="flex min-w-32 flex-1 flex-col gap-1">
      <label for="manualMmsPort" class="text-sm font-medium text-slate-700 dark:text-slate-300">MMS Port</label>
      <input
        id="manualMmsPort"
        v-model="mmsPortInput"
        type="number"
        min="1"
        max="65535"
        step="1"
        :disabled="props.disabled"
        class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
      <p v-if="portError" class="text-xs text-red-600 dark:text-red-400">{{ portError }}</p>
    </div>

    <div class="flex min-w-40 flex-1 flex-col gap-1">
      <label for="manualInterfaceId" class="text-sm font-medium text-slate-700 dark:text-slate-300">Interface</label>
      <input
        id="manualInterfaceId"
        v-model="interfaceId"
        type="text"
        placeholder="eth0"
        :disabled="props.disabled"
        class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
      <p v-if="interfaceError" class="text-xs text-red-600 dark:text-red-400">{{ interfaceError }}</p>
    </div>

    <div class="flex min-w-36 flex-1 flex-col gap-1">
      <label for="manualIedName" class="text-sm font-medium text-slate-700 dark:text-slate-300">IED Name</label>
      <input
        id="manualIedName"
        v-model="iedName"
        type="text"
        placeholder="optional"
        :disabled="props.disabled"
        class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
    </div>

    <div class="flex min-w-full flex-col gap-1 sm:min-w-72 sm:flex-1">
      <StructureFileUpload :disabled="props.disabled" @update:path="handleStructureFilePath" />
      <p v-if="iedNameError" class="text-xs text-red-600 dark:text-red-400">{{ iedNameError }}</p>
    </div>

    <Button type="submit" variant="primary" :icon="Plug" :disabled="props.disabled" @click="captureShiftKey">
      Connect
    </Button>
  </form>
</template>
