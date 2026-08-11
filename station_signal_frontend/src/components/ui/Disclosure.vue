<script setup lang="ts">
import { computed, ref } from 'vue'
import { ChevronDown } from '@lucide/vue'

import { useI18n } from '@/i18n'

const props = withDefaults(
  defineProps<{
    /** Button text when collapsed. Defaults to the shared "Advanced" label. */
    label?: string
    /** Optional one-line summary of what's hidden, e.g. "eth0 · port 102". */
    summary?: string
    /**
     * Forces the body open regardless of the user's own toggle. Callers pass a validation-error
     * condition here so a field can never sit invalid inside a collapsed section — the whole
     * point of hiding always-defaulted fields is that they're *correct*, and an errored one
     * no longer is.
     */
    forceOpen?: boolean
  }>(),
  { label: undefined, summary: undefined, forceOpen: false },
)

const userOpen = ref(false)
const isOpen = computed(() => props.forceOpen || userOpen.value)

const { t } = useI18n()
const buttonLabel = computed(() => props.label ?? t('common.advanced'))
const ariaLabel = computed(() => (isOpen.value ? t('common.hideAdvanced') : t('common.showAdvanced')))
</script>

<template>
  <div class="flex w-full flex-col gap-2">
    <div class="flex items-center gap-2">
      <button
        type="button"
        class="flex items-center gap-1 rounded-md px-1.5 py-1 text-xs font-medium text-slate-500 hover:bg-slate-100 hover:text-slate-700 disabled:cursor-not-allowed disabled:opacity-60 dark:text-slate-400 dark:hover:bg-slate-800 dark:hover:text-slate-200"
        :aria-expanded="isOpen"
        :aria-label="ariaLabel"
        :disabled="props.forceOpen"
        @click="userOpen = !userOpen"
      >
        <ChevronDown :size="13" class="transition-transform" :class="isOpen ? 'rotate-180' : ''" />
        {{ buttonLabel }}
      </button>
      <span v-if="summary && !isOpen" class="font-mono text-xs text-slate-400 dark:text-slate-500">
        {{ summary }}
      </span>
    </div>

    <!-- v-show, not v-if: the hidden fields keep their state and their v-model bindings stay
         live, so collapsing a section never silently resets what's inside it. -->
    <div v-show="isOpen" class="flex flex-wrap items-end gap-4">
      <slot />
    </div>
  </div>
</template>
