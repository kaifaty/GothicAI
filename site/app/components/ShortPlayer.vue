<!-- The short: a YouTube video behind a click (nothing loads from YouTube before it), else a muted loop that
loads only when it comes near the screen and plays unless the reader asked for less motion (site.js), else
the still. Behind it the same still, blurred, lights the page around the frame. `poster` is under the
site's root (/static/hero/wrath.jpg), `loop` a full link. -->
<script setup lang="ts">
const props = withDefaults(defineProps<{ youtube?: string; poster?: string; loop?: string; label: string; alt: string; eager?: boolean }>(), {
  youtube: "",
  poster: "",
  loop: "",
});
const $img = useImage();
// the video's still is one picture (a poster takes no srcset): the frame's own 540 px, as WebP at a
// quality a moving frame forgives
const still = props.poster ? $img(props.poster, { width: 540, format: "webp", quality: 70 }) : "";
const frame = { width: 540, height: 960, widths: [320, 540], sizes: "20rem", loading: props.eager ? undefined : ("lazy" as const) };
</script>

<template>
  <div v-if="youtube || loop || poster" class="short">
    <!-- the glow is blurred by 56 px: a small copy looks the same -->
    <SitePicture v-if="poster" class="glow" single :src="poster" :width="540" :height="960" :widths="[120]" aria-hidden="true" loading="lazy" decoding="async" />
    <button v-if="youtube" type="button" class="yt" :data-id="youtube" :aria-label="label"><SitePicture v-if="poster" :src="poster" v-bind="frame" :alt="alt" decoding="async" /><span class="play" aria-hidden="true" /></button>
    <video v-else-if="loop" class="loop" muted loop playsinline preload="none" :poster="still || undefined" :aria-label="alt" width="540" height="960"><source :data-src="loop" type="video/mp4"></video>
    <SitePicture v-else :src="poster" single v-bind="frame" :alt="alt" decoding="async" />
  </div>
</template>
