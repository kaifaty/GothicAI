<!-- One page for both languages: GitHub Pages serves 404.html for any missing address. -->
<script setup lang="ts">
const { url, base } = useLinks();
const en = UI.en;
const ru = UI.ru;
</script>

<template>
  <SitePage lang="en" title="404" :here="`${base}404.html`" :other="url('ru')" cls="lost-page">
    <section class="lost">
      <img class="lost-seal" :src="`${base}static/brand/favicon.svg`" width="96" height="96" alt="">
      <p class="eyebrow">404</p>
      <h1>{{ en.not_found }}</h1>
      <p class="lede" lang="ru">{{ ru.not_found }}</p>
      <p class="actions"><a class="button" :href="url('en')">{{ en.to_home }}</a><a class="arrow" :href="url('ru')" lang="ru">{{ ru.to_home }}</a></p>
    </section>
  </SitePage>
</template>
