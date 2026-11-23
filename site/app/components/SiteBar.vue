<!-- The bar that floats over the page: the mark, the devlog, the channel, the code and the other language. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang; other: string; current?: string }>();
const { url, base } = useLinks();
const ui = UI[props.lang];
const s = site.settings;
const name = s.title ?? "Gothic AI";
</script>

<template>
  <header class="bar">
    <div class="bar-in">
      <a class="mark" :href="url(lang)"><picture><source media="(max-width: 30rem)" :srcset="`${base}static/brand/favicon.svg`"><img :src="`${base}static/brand/lockup.svg`" width="127" height="32" :alt="name"></picture></a>
      <nav :aria-label="ui.menu"><a :href="url(lang, 'posts/')" :aria-current="current === 'devlog' ? 'page' : undefined">{{ ui.devlog }}</a><a v-if="s.telegram" :href="s.telegram">{{ ui.telegram }}</a><a v-if="s.repo" :href="s.repo">{{ ui.source }}</a><a :href="other" :hreflang="ui.other_lang" :lang="ui.other_lang">{{ ui.other }}</a></nav>
    </div>
  </header>
</template>
