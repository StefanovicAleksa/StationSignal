<script setup lang="ts">
import { ref } from 'vue'
import { Upload } from '@lucide/vue'

import { uploadStructureFile } from '@/services/structureFileApi'
import { ApiError } from '@/types/api'
import Button from '@/components/ui/Button.vue'

const props = defineProps<{
  disabled: boolean
}>()

const emit = defineEmits<{
  'update:path': [path: string | undefined]
}>()

const fileInput = ref<HTMLInputElement>()
const selectedFileName = ref<string | null>(null)
const uploading = ref(false)
const error = ref<string | null>(null)
const isDragOver = ref(false)

function onBrowseClick() {
  if (props.disabled || uploading.value) return
  fileInput.value?.click()
}

function onDragOver() {
  if (props.disabled || uploading.value) return
  isDragOver.value = true
}

function onDragLeave() {
  isDragOver.value = false
}

function onDrop(event: DragEvent) {
  isDragOver.value = false
  if (props.disabled || uploading.value) return
  const file = event.dataTransfer?.files?.[0]
  if (file) handleFile(file)
}

function onInputChange(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (file) handleFile(file)
}

async function handleFile(file: File) {
  error.value = null
  uploading.value = true
  try {
    const result = await uploadStructureFile(file)
    selectedFileName.value = file.name
    emit('update:path', result.path)
  } catch (err) {
    selectedFileName.value = null
    emit('update:path', undefined)
    error.value = err instanceof ApiError ? err.message : 'Failed to upload structure file.'
  } finally {
    uploading.value = false
  }
}

function clear() {
  selectedFileName.value = null
  error.value = null
  emit('update:path', undefined)
}
</script>

<template>
  <div class="flex flex-col gap-1">
    <label class="text-sm font-medium text-slate-700 dark:text-slate-300">Structure File (SCL/ICD/CID)</label>
    <div
      class="flex w-full flex-col items-center justify-center gap-1 rounded-md border-2 border-dashed px-3 py-3 text-center text-xs transition-colors sm:w-72"
      :class="[
        isDragOver ? 'border-blue-500 bg-blue-50 dark:bg-blue-900/20' : 'border-slate-300 dark:border-slate-700',
        props.disabled
          ? 'cursor-not-allowed bg-slate-100 text-slate-400 dark:bg-slate-800 dark:text-slate-500'
          : 'cursor-pointer text-slate-500 hover:border-blue-400 dark:text-slate-400',
      ]"
      @click="onBrowseClick"
      @dragover.prevent="onDragOver"
      @dragleave.prevent="onDragLeave"
      @drop.prevent="onDrop"
    >
      <input
        ref="fileInput"
        type="file"
        class="hidden"
        accept=".icd,.cid,.scd,.xml"
        :disabled="props.disabled"
        @change="onInputChange"
      />
      <template v-if="uploading">
        <span>Uploading…</span>
      </template>
      <template v-else-if="selectedFileName">
        <span class="break-all font-medium text-slate-700 dark:text-slate-300">{{ selectedFileName }}</span>
        <Button variant="ghost" size="sm" @click.stop="clear">Remove</Button>
      </template>
      <template v-else>
        <Upload :size="16" class="text-slate-400 dark:text-slate-500" />
        <span>Drag &amp; drop a structure file here, or click to browse</span>
        <span class="text-slate-400 dark:text-slate-500">.icd, .cid, .scd, .xml</span>
      </template>
    </div>
    <p v-if="error" class="text-xs text-red-600 dark:text-red-400">{{ error }}</p>
  </div>
</template>
