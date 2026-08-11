<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Plug } from '@lucide/vue'

import StructureFileUpload from '@/components/device/StructureFileUpload.vue'
import Button from '@/components/ui/Button.vue'
import Disclosure from '@/components/ui/Disclosure.vue'
import ShortcutHint from '@/components/ui/ShortcutHint.vue'
import { useSettingsStore } from '@/stores/settings'
import { useI18n } from '@/i18n'
import { connectPresetForEvent, type ConnectPreset } from '@/utils/connectPreset'

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
    preset: ConnectPreset,
  ]
}>()

// Defaults to the box's own active network interface (from the Settings page's network status)
// rather than a hardcoded name — see ScanForm.vue's identical rationale.
const settingsStore = useSettingsStore()
const { t } = useI18n()

const host = ref('')
const mmsPortInput = ref('102')
const interfaceId = ref('')
const iedName = ref('')
const sclFilePath = ref('')
const touched = ref(false)

// Captured from the Connect button's own click (a form 'submit' event carries no modifier-key
// state) - click fires before submit, so this is always up to date by the time handleSubmit runs.
// See connectPreset.ts for what each modifier means.
const lastClickPreset = ref<ConnectPreset>('ask')

function captureModifiers(event: MouseEvent) {
  lastClickPreset.value = connectPresetForEvent(event)
}

const shortcutItems = computed(() => [
  { key: 'Shift', text: t('shortcuts.connectDefault') },
  { key: 'Ctrl', text: t('shortcuts.connectAll') },
])

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
  return host.value.trim().length === 0 ? t('fields.hostRequired') : null
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

// Mirrors the daemon's own rule: a structure file is only meaningful together with the IED
// name it defines, so require both or neither (see AGENT_API_GUIDE.md's START_REPORTING params).
const iedNameError = computed(() => {
  if (!touched.value) return null
  return sclFilePath.value.trim().length > 0 && iedName.value.trim().length === 0
    ? t('structureFile.iedNameRequired')
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
    lastClickPreset.value,
  )
}

function handleStructureFilePath(path: string | undefined) {
  sclFilePath.value = path ?? ''
}

// Everything except Host is either prefilled (interface), a fixed default (port 102), or purely
// optional (IED name, structure file), so the form opens as a one-field form. Force-opened as
// soon as anything inside it is invalid.
const advancedSummary = computed(() => `${interfaceId.value || '—'} · :${mmsPortInput.value}`)
const advancedForceOpen = computed(
  () =>
    Boolean(interfaceError.value || portError.value || iedNameError.value) ||
    interfaceId.value.trim().length === 0,
)
</script>

<template>
  <form class="flex flex-col gap-3" @submit.prevent="handleSubmit">
    <div class="flex flex-wrap items-end gap-4">
      <div class="flex min-w-40 flex-1 flex-col gap-1">
        <label for="manualHost" class="text-sm font-medium text-slate-700 dark:text-slate-300">
          {{ t('fields.host') }}
        </label>
        <input
          id="manualHost"
          v-model="host"
          type="text"
          :placeholder="t('fields.hostPlaceholder')"
          :disabled="props.disabled"
          class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
        />
        <p v-if="hostError" class="text-xs text-red-600 dark:text-red-400">{{ hostError }}</p>
      </div>

      <Button
        type="submit"
        variant="primary"
        :icon="Plug"
        :disabled="props.disabled"
        :title="t('shortcuts.connectTooltip')"
        @click="captureModifiers"
      >
        {{ t('common.connect') }}
      </Button>
    </div>

    <Disclosure :summary="advancedSummary" :force-open="advancedForceOpen">
      <div class="flex min-w-32 flex-1 flex-col gap-1">
        <label for="manualMmsPort" class="text-sm font-medium text-slate-700 dark:text-slate-300">
          {{ t('fields.mmsPort') }}
        </label>
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
        <label for="manualInterfaceId" class="text-sm font-medium text-slate-700 dark:text-slate-300">
          {{ t('fields.interface') }}
        </label>
        <input
          id="manualInterfaceId"
          v-model="interfaceId"
          type="text"
          :placeholder="t('fields.interfacePlaceholder')"
          :disabled="props.disabled"
          class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
        />
        <p v-if="interfaceError" class="text-xs text-red-600 dark:text-red-400">{{ interfaceError }}</p>
      </div>

      <div class="flex min-w-36 flex-1 flex-col gap-1">
        <label for="manualIedName" class="text-sm font-medium text-slate-700 dark:text-slate-300">
          {{ t('fields.iedName') }}
        </label>
        <input
          id="manualIedName"
          v-model="iedName"
          type="text"
          :placeholder="t('fields.optional')"
          :disabled="props.disabled"
          class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
        />
      </div>

      <div class="flex min-w-full flex-col gap-1 sm:min-w-72 sm:flex-1">
        <StructureFileUpload :disabled="props.disabled" @update:path="handleStructureFilePath" />
        <p v-if="iedNameError" class="text-xs text-red-600 dark:text-red-400">{{ iedNameError }}</p>
      </div>
    </Disclosure>

    <ShortcutHint :items="shortcutItems" />
  </form>
</template>
