<script setup lang="ts">
withDefaults(
  defineProps<{
    align?: 'left' | 'right'
  }>(),
  { align: 'left' },
)
</script>

<template>
  <th
    class="px-3 py-1.5 font-medium text-slate-600 dark:text-slate-400"
    :class="align === 'right' ? 'text-right' : 'text-left'"
  >
    <slot />
  </th>
</template>
