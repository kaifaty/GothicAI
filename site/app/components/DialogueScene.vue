<!-- The conversation from the game as a play's script, beside the take it comes from. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang }>();
const { base } = useLinks();
const h = site.home[props.lang];
const hero = site.hero;
const lines = (h.dialogue ?? []) as { hero?: boolean; name: string; text: string }[];
const poster = hero.poster ? `/static/${hero.poster}` : "";
const loop = hero[`loop_${props.lang}`] ? `${base}static/${hero[`loop_${props.lang}`]}` : "";
</script>

<template>
  <section class="scene">
    <div class="scene-in">
      <div class="scene-head">
        <ShortPlayer :youtube="hero.youtube ?? ''" :poster="poster" :loop="loop" :label="UI[lang].play" :alt="h.video_alt ?? ''" />
        <p class="eyebrow">{{ h.dialogue_label ?? "" }}</p>
        <p class="note">{{ h.dialogue_note ?? "" }}</p>
      </div>
      <div class="dialogue" :aria-label="h.dialogue_label ?? ''">
        <p v-for="(line, i) in lines" :key="i" :class="['line', line.hero ? 'by-hero' : 'by-npc', { last: i === lines.length - 1 }]" data-reveal><span class="who">{{ line.name }}</span><span class="said">{{ line.text }}</span></p>
      </div>
    </div>
  </section>
</template>
