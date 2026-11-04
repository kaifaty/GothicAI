<!-- How a conversation goes: a page of the hero's diary, torn out, the steps drawn as a track in ink and wax. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang }>();
const h = site.home[props.lang];
const steps = ((h.how_steps ?? []) as ({ label?: string; text: string } | string)[]).map((step, i) => ({
  n: String(i + 1).padStart(2, "0"),
  ...(typeof step === "string" ? { label: "", text: step } : { label: step.label ?? "", text: step.text }),
}));
</script>

<template>
  <section class="how">
    <div class="diary">
      <SectionHead :title="h.how_title" />
      <p class="how-text">{{ h.how_text }}</p>
      <ol class="flow" data-reveal><li v-for="step in steps" :key="step.n"><span class="step-n">{{ step.n }}</span><h3 v-if="step.label">{{ step.label }}</h3><p>{{ step.text }}</p></li></ol>
    </div>
  </section>
</template>
