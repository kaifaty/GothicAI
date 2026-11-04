<!-- A section's heading: a small label over the title, and a link on the right (the `more` slot). -->
<script setup lang="ts">
withDefaults(defineProps<{ title: string; eyebrow?: string; level?: 1 | 2 }>(), { eyebrow: "", level: 2 });
</script>

<template>
  <header class="section-head"><div><p v-if="eyebrow" class="eyebrow">{{ eyebrow }}</p><component :is="`h${level}`">{{ title }}</component></div><slot /></header>
</template>
