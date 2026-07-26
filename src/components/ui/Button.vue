<script setup lang="ts">
import { computed, type FunctionalComponent } from 'vue'

const props = withDefaults(
  defineProps<{
    variant?: 'primary' | 'secondary' | 'danger' | 'ghost'
    size?: 'sm' | 'md'
    icon?: FunctionalComponent
    disabled?: boolean
    type?: 'button' | 'submit'
  }>(),
  { variant: 'secondary', size: 'md', disabled: false, type: 'button' },
)

const base =
  'inline-flex items-center justify-center gap-1.5 rounded-md font-medium transition-colors disabled:cursor-not-allowed disabled:opacity-50'

const sizeClasses: Record<string, string> = {
  sm: 'px-2 py-1 text-xs',
  md: 'px-3 py-1.5 text-sm',
}

const variantClasses: Record<string, string> = {
  primary:
    'bg-blue-600 text-white shadow-sm hover:bg-blue-700 disabled:hover:bg-blue-600 dark:bg-blue-600 dark:hover:bg-blue-500',
  secondary:
    'border border-slate-300 bg-white text-slate-700 hover:bg-slate-100 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-200 dark:hover:bg-slate-800',
  danger:
    'bg-red-600 text-white shadow-sm hover:bg-red-700 disabled:hover:bg-red-600 dark:bg-red-600 dark:hover:bg-red-500',
  ghost:
    'text-slate-600 hover:bg-slate-100 hover:text-slate-900 dark:text-slate-300 dark:hover:bg-slate-800 dark:hover:text-slate-100',
}

const classes = computed(() => [base, sizeClasses[props.size], variantClasses[props.variant]])

const iconSize = computed(() => (props.size === 'sm' ? 13 : 15))
</script>

<template>
  <button :type="type" :disabled="disabled" :class="classes">
    <component :is="icon" v-if="icon" :size="iconSize" />
    <slot />
  </button>
</template>
