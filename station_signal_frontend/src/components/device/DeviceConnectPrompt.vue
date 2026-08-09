<script setup lang="ts">
import { ref } from 'vue'
import { Plug } from '@lucide/vue'

import { useDevicesStore } from '@/stores/devices'
import { ApiError, DEFAULT_LN_CATEGORIES, formatApiErrorDetail, isAuthRequiredError, type LnCategory } from '@/types/api'
import Button from '@/components/ui/Button.vue'
import ConnectCategoryModal from '@/components/device/ConnectCategoryModal.vue'

const emit = defineEmits<{
  connected: [key: string]
  error: [message: string]
}>()

const devicesStore = useDevicesStore()

interface PendingPasswordPrompt {
  host: string
  mmsPort: number
  interfaceId: string
  iedName?: string
  sclFilePath?: string
  lnCategories?: LnCategory[]
}

const pendingPasswordPrompt = ref<PendingPasswordPrompt | null>(null)
const passwordInput = ref('')
const passwordSubmitting = ref(false)
const passwordTouched = ref(false)

// Actually calls the API. Separated from connect() below so a password retry
// (submitPassword) can re-invoke this directly without re-showing the category picker -
// the categories for this connect attempt were already decided once, either by the modal
// or by a Shift-bypass.
async function doStart(
  host: string,
  mmsPort: number,
  interfaceId: string,
  acseAuthPassword: string | undefined,
  iedName: string | undefined,
  sclFilePath: string | undefined,
  lnCategories: LnCategory[] | undefined,
) {
  try {
    const key = await devicesStore.startDevice({
      host,
      mmsPort,
      interfaceId,
      acseAuthPassword,
      iedName,
      sclFilePath,
      lnCategories,
    })
    pendingPasswordPrompt.value = null
    passwordInput.value = ''
    emit('connected', key)
  } catch (err) {
    if (isAuthRequiredError(err)) {
      pendingPasswordPrompt.value = { host, mmsPort, interfaceId, iedName, sclFilePath, lnCategories }
      return
    }
    pendingPasswordPrompt.value = null
    emit('error', err instanceof ApiError ? formatApiErrorDetail(err) : 'Failed to connect to device.')
  }
}

interface PendingCategoryPrompt {
  host: string
  mmsPort: number
  interfaceId: string
  acseAuthPassword?: string
  iedName?: string
  sclFilePath?: string
}

const pendingCategoryPrompt = ref<PendingCategoryPrompt | null>(null)
const categoryModalOpen = ref(false)

// Exposed so callers (ScanView, DevicesView) can trigger a connect attempt from wherever they
// get host/mmsPort/interfaceId (a scan result, a manual form, ...) without this component
// needing to know where that came from.
//
// bypassCategoryModal is true when the user held Shift while clicking Connect - skips the
// category picker entirely and connects straight away with the same default categories the
// picker itself would have pre-checked (DEFAULT_LN_CATEGORIES), a shortcut for the common case,
// not an "unfiltered" escape hatch.
async function connect(
  host: string,
  mmsPort: number,
  interfaceId: string,
  acseAuthPassword?: string,
  iedName?: string,
  sclFilePath?: string,
  bypassCategoryModal = false,
) {
  if (bypassCategoryModal) {
    await doStart(host, mmsPort, interfaceId, acseAuthPassword, iedName, sclFilePath, DEFAULT_LN_CATEGORIES)
    return
  }
  pendingCategoryPrompt.value = { host, mmsPort, interfaceId, acseAuthPassword, iedName, sclFilePath }
  categoryModalOpen.value = true
}

async function onCategoryConfirm(categories: LnCategory[] | undefined) {
  categoryModalOpen.value = false
  const pending = pendingCategoryPrompt.value
  pendingCategoryPrompt.value = null
  if (!pending) return
  await doStart(
    pending.host,
    pending.mmsPort,
    pending.interfaceId,
    pending.acseAuthPassword,
    pending.iedName,
    pending.sclFilePath,
    categories,
  )
}

function onCategoryCancel() {
  categoryModalOpen.value = false
  pendingCategoryPrompt.value = null
}

async function submitPassword() {
  passwordTouched.value = true
  const pending = pendingPasswordPrompt.value
  if (!pending || passwordInput.value.trim().length === 0) return

  passwordSubmitting.value = true
  try {
    await doStart(
      pending.host,
      pending.mmsPort,
      pending.interfaceId,
      passwordInput.value,
      pending.iedName,
      pending.sclFilePath,
      pending.lnCategories,
    )
  } finally {
    passwordSubmitting.value = false
    passwordTouched.value = false
  }
}

function cancel() {
  pendingPasswordPrompt.value = null
  passwordInput.value = ''
  passwordTouched.value = false
}

defineExpose({ connect })
</script>

<template>
  <ConnectCategoryModal :open="categoryModalOpen" @confirm="onCategoryConfirm" @cancel="onCategoryCancel" />

  <form
    v-if="pendingPasswordPrompt"
    class="flex flex-wrap items-end gap-4 rounded-md border border-amber-300 bg-amber-50 px-4 py-3 dark:border-amber-800 dark:bg-amber-900/30"
    @submit.prevent="submitPassword"
  >
    <div class="flex flex-col gap-1">
      <label for="acseAuthPassword" class="text-sm font-medium text-slate-700 dark:text-slate-300">
        {{ pendingPasswordPrompt.host }} requires a password
      </label>
      <input
        id="acseAuthPassword"
        v-model="passwordInput"
        type="password"
        autofocus
        :disabled="passwordSubmitting"
        class="w-56 rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
      <p v-if="passwordTouched && passwordInput.trim().length === 0" class="text-xs text-red-600 dark:text-red-400">
        Password is required.
      </p>
    </div>

    <Button type="submit" variant="primary" :icon="Plug" :disabled="passwordSubmitting">Connect</Button>
    <Button type="button" variant="secondary" :disabled="passwordSubmitting" @click="cancel">Cancel</Button>
  </form>
</template>
