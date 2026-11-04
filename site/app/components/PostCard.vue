<!-- One post on a card: its cover (a frame of the short, ~280 px wide), the date, the title, the summary. -->
<script setup lang="ts">
type Post = (typeof site.posts)[number];
const props = defineProps<{ post: Post; lang: Lang; level: 2 | 3 }>();
const { url, base } = useLinks();
const ui = UI[props.lang];
const v = props.post.versions[props.lang]!;
</script>

<template>
  <li data-reveal><a class="card" :href="url(lang, `posts/${post.slug}/`)"><span class="pic"><SitePicture v-if="v.cover" :src="`/media/${post.slug}/${v.cover}`" :width="540" :height="960" :widths="[280, 560]" density loading="lazy" decoding="async" /><img v-else class="seal" :src="`${base}static/brand/favicon.svg`" width="40" height="40" alt=""><span v-if="v.youtube" class="badge">{{ ui.short }}</span></span><time :datetime="post.day">{{ post.date[lang] }}</time><component :is="`h${level}`">{{ v.title }}</component><span class="sum">{{ v.summary }}</span></a></li>
</template>
