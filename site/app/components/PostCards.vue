<!-- The posts as cards: the cover, the date, the title and the summary, newest first. -->
<script setup lang="ts">
const props = withDefaults(defineProps<{ lang: Lang; limit?: number; level?: 2 | 3; cls?: string }>(), { level: 2, cls: "cards" });
const ui = UI[props.lang];
const posts = site.posts.filter((p) => p.versions[props.lang]).slice(0, props.limit);
</script>

<template>
  <ol v-if="posts.length" :class="cls" reversed>
    <PostCard v-for="post in posts" :key="post.slug" :post="post" :lang="lang" :level="level" />
  </ol>
  <p v-else class="empty">{{ ui.empty }}</p>
</template>
