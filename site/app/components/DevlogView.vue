<!-- The devlog: every post in this language as a card, newest first. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang }>();
const { url } = useLinks();
const h = site.home[props.lang];
const ui = UI[props.lang];
</script>

<template>
  <SitePage :lang="lang" :title="h.devlog_title" :here="url(lang, 'posts/')" :other="url(otherLang(lang), 'posts/')" current="devlog">
    <section class="devlog">
      <SectionHead :title="h.devlog_title" :level="1"><a class="arrow" :href="url(lang, 'feed.xml')">{{ ui.feed }}</a></SectionHead>
      <p v-if="h.devlog_lede" class="lede">{{ h.devlog_lede }}</p>
      <PostCards :lang="lang" />
    </section>
  </SitePage>
</template>
