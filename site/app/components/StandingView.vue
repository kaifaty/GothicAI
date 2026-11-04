<!-- A page outside the devlog (the privacy policy): its title, its summary and its text. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang; slug: string }>();
const { url } = useLinks();
const page = site.pages.find((p) => p.slug === props.slug);
const v = page?.versions[props.lang];
if (!page || !v) throw createError({ statusCode: 404, statusMessage: "Not Found", fatal: true });
const other = otherLang(props.lang);
</script>

<template>
  <SitePage
    :lang="lang"
    :title="v!.title"
    :here="url(lang, `${slug}/`)"
    :other="url(other, page!.versions[other] ? `${slug}/` : '')"
    :description="v!.summary"
    cls="article"
  >
    <article class="post">
      <header class="post-head">
        <h1>{{ v!.title }}</h1>
        <p class="lede">{{ v!.summary }}</p>
      </header>
      <div class="post-grid"><div class="text" v-html="v!.html" /></div>
    </article>
  </SitePage>
</template>
