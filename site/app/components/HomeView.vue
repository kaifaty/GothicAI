<!-- The home page: the valley and the pitch, the conversation and its take, the claim, the people, what
works, how a conversation goes, and the newest posts. -->
<script setup lang="ts">
const props = defineProps<{ lang: Lang }>();
const { url } = useLinks();
const h = site.home[props.lang];
const ui = UI[props.lang];
const repo = site.settings.repo as string | undefined;
const features = (h.features ?? []) as { title: string; text: string }[];
</script>

<template>
  <SitePage :lang="lang" :title="site.settings.title ?? 'Gothic AI'" :here="url(lang)" :other="url(otherLang(lang))" cls="home">
    <section class="hero">
      <!-- the first screen is the valley under the Barrier; on a phone a picture of its own, no wider than 800 px -->
      <div class="backdrop" aria-hidden="true"><SitePicture :src="art('valley')" :width="1672" :height="941" :widths="[1280, 1672]" sizes="100vw" :phone="{ media: '(max-width: 59.99rem)', rest: '(min-width: 60rem)', widths: [800], sizes: '100vw' }" fetchpriority="high" preload /></div>
      <div class="hero-in">
        <div class="pitch">
          <p class="eyebrow">{{ h.eyebrow ?? "" }}</p>
          <h1>{{ h.headline }}</h1>
          <p class="lede">{{ h.lede }}</p>
          <p class="actions"><a class="button" :href="url(lang, 'posts/')">{{ h.read_devlog }}</a><a v-if="repo" class="arrow" :href="repo">{{ h.see_code }}</a></p>
          <p class="status">{{ h.status ?? "" }}</p>
        </div>
      </div>
    </section>
    <DialogueScene :lang="lang" />
    <!-- the old mage looks on beside the claim -->
    <section v-if="h.claim" class="claim"><div class="claim-in"><p class="claim-text" data-words>{{ h.claim }}</p><SitePicture class="portrait" single :src="art('mage')" :width="768" :height="1024" :widths="[384, 768]" sizes="(min-width: 64rem) 24rem, 20rem" loading="lazy" decoding="async" /></div></section>
    <ColonyPeople :lang="lang" />
    <section class="works">
      <SectionHead :title="h.features_title" />
      <ol class="features"><li v-for="(f, i) in features" :key="f.title" data-reveal><span class="num">{{ ROMAN[i % ROMAN.length] }}</span><h3>{{ f.title }}</h3><p>{{ f.text }}</p></li></ol>
    </section>
    <DiaryFlow :lang="lang" />
    <section class="log">
      <SectionHead :title="h.devlog_title"><a class="arrow" :href="url(lang, 'posts/')">{{ ui.all_posts }}</a></SectionHead>
      <PostCards :lang="lang" :limit="4" :level="3" cls="cards shelf" />
    </section>
  </SitePage>
</template>
