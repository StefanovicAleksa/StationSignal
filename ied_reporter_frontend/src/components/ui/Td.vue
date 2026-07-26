<script setup lang="ts">
withDefaults(
  defineProps<{
    align?: 'left' | 'right'
    muted?: boolean
    mono?: boolean
    nowrap?: boolean
  }>(),
  { align: 'left', muted: false, mono: false, nowrap: false },
)
</script>

<template>
  <td
    class="px-3 py-1.5"
    :class="[
      align === 'right' ? 'text-right' : 'text-left',
      muted ? 'text-slate-400 dark:text-slate-500' : 'text-slate-700 dark:text-slate-300',
      mono ? 'font-mono' : '',
      nowrap ? 'whitespace-nowrap' : '',
    ]"
  >
    <slot />
  </td>
</template>
