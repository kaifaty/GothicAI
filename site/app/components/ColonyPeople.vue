<!-- The people of the colony: a row of portraits with their names, marked as illustrations. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang }>();
const h = site.home[props.lang];
const people = ((h.people ?? []) as string[]).map((name, i) => ({ name, src: art(PEOPLE[i % PEOPLE.length]!) }));
</script>

<template>
  <section v-if="people.length" class="people">
    <SectionHead :eyebrow="h.people_label ?? ''" :title="h.people_title ?? ''" />
    <div class="people-row"><figure v-for="p in people" :key="p.name" data-reveal><SitePicture :src="p.src" :width="768" :height="1024" :widths="[320, 640, 768]" sizes="(min-width: 64rem) 18rem, 50vw" loading="lazy" decoding="async" /><figcaption>{{ p.name }}</figcaption></figure></div>
    <p v-if="h.art_note" class="art-note">{{ h.art_note }}</p>
  </section>
</template>
