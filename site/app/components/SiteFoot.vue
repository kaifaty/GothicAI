<!-- The footer: where the project stands, the feed, the code, the standing pages, the other language and
the credit, over the valley sinking into the night. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang; other: string }>();
const { url, base } = useLinks();
const ui = UI[props.lang];
const home = site.home[props.lang];
const pages = site.pages.map((p) => {
  const at: Lang = p.versions[props.lang] ? props.lang : "en";
  return { href: url(at, `${p.slug}/`), title: p.versions[at]!.title };
});
</script>

<template>
  <footer class="foot">
    <div class="foot-in">
      <img class="foot-seal" :src="`${base}static/brand/favicon.svg`" width="40" height="40" alt="">
      <p class="foot-status">{{ home.status ?? "" }}</p>
      <nav class="foot-links"><a :href="url(lang, 'feed.xml')">{{ ui.feed }}</a><a v-if="site.settings.repo" :href="site.settings.repo">GitHub</a><a v-for="p in pages" :key="p.href" :href="p.href">{{ p.title }}</a><a :href="other" :hreflang="ui.other_lang" :lang="ui.other_lang">{{ ui.other }}</a></nav>
      <p v-if="home.credit" class="credit" v-html="home.credit" />
    </div>
  </footer>
</template>
