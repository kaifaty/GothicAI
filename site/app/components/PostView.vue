<!-- A post: its title and summary, the short beside the text, and the way to its neighbours. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang; slug: string }>();
const { url, base } = useLinks();
const ui = UI[props.lang];
const posts = site.posts.filter((p) => p.versions[props.lang]);
const i = posts.findIndex((p) => p.slug === props.slug);
if (i < 0) throw createError({ statusCode: 404, statusMessage: "Not Found", fatal: true });
const post = posts[i]!;
const v = post.versions[props.lang]!;
const other = otherLang(props.lang);
const poster = v.cover ? `/media/${post.slug}/${v.cover}` : "";
const neighbours = [
  { post: i > 0 ? posts[i - 1] : undefined, cls: "newer" as const },
  { post: posts[i + 1], cls: "older" as const },
].filter((n) => n.post);
</script>

<template>
  <SitePage
    :lang="lang"
    :title="v.title"
    :here="url(lang, `posts/${post.slug}/`)"
    :other="post.versions[other] ? url(other, `posts/${post.slug}/`) : url(other, 'posts/')"
    :description="v.summary"
    cls="article"
    current="devlog"
    :image="poster ? `${base}${poster.slice(1)}` : ''"
  >
    <article class="post">
      <header class="post-head">
        <p class="eyebrow"><a :href="url(lang, 'posts/')">{{ ui.devlog }}</a><span aria-hidden="true">·</span><time :datetime="post.day">{{ post.date[lang] }}</time></p>
        <h1>{{ v.title }}</h1>
        <p class="lede">{{ v.summary }}</p>
      </header>
      <div :class="['post-grid', { 'has-short': v.youtube || v.cover }]">
        <aside v-if="v.youtube || v.cover" class="post-short"><ShortPlayer :youtube="v.youtube" :poster="poster" :label="ui.play" :alt="v.title" eager /></aside>
        <div class="text" v-html="v.html" />
      </div>
      <nav class="post-nav"><a v-for="n in neighbours" :key="n.cls" :class="n.cls" :href="url(lang, `posts/${n.post!.slug}/`)"><span class="eyebrow">{{ ui[n.cls] }}</span><span class="to">{{ n.post!.versions[lang]!.title }}</span></a></nav><p class="back"><a class="arrow back-arrow" :href="url(lang, 'posts/')">{{ ui.back }}</a></p>
    </article>
  </SitePage>
</template>
