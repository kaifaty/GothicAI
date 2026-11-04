<!-- A picture made smaller when the site is built (@nuxt/image): AVIF with a WebP fallback in a <picture>,
in the widths the layout needs. `phone` gives narrow screens a picture of their own (the first screen's
valley); `single` is a plain WebP <img> where a <picture> around it would break a CSS rule (`.short > img`)
or the grid it sits in (`.portrait`). `width` and `height` are the picture's own size: no width beyond it
is asked for. -->
<script setup lang="ts">
const props = withDefaults(
  defineProps<{
    src: string; // under the site's root: /static/art/valley.webp, /media/<slug>/cover.jpg
    width: number;
    height: number;
    widths: number[]; // the candidates, in CSS pixels at 1x when `density`
    sizes?: string;
    density?: boolean; // widths are 1x and 2x of one CSS size
    phone?: { media: string; widths: number[]; sizes?: string; rest: string }; // rest: the media query of the other screens
    single?: boolean;
    preload?: boolean;
    alt?: string;
    loading?: "lazy" | "eager";
    decoding?: "async";
    fetchpriority?: "high";
  }>(),
  { alt: "" },
);

const QUALITY = { avif: 55, webp: 78 } as const;
type Format = keyof typeof QUALITY;
const $img = useImage();
const url = (w: number, format: Format) => $img(props.src, { width: Math.min(w, props.width), format, quality: QUALITY[format] });
const srcset = (widths: number[], format: Format, density = false) =>
  [...new Set(widths.map((w) => Math.min(w, props.width)))]
    .map((w, i) => `${url(w, format)} ${density ? `${i + 1}x` : `${w}w`}`)
    .join(", ");

const largest = Math.max(...props.widths);
const sizes = props.density ? undefined : props.sizes;
// (a plain <img> has no sources: only the WebP widths are made)
const sources = props.single
  ? []
  : [
      ...(props.phone
        ? (["avif", "webp"] as const).map((f) => ({ media: props.phone!.media, type: `image/${f}`, srcset: srcset(props.phone!.widths, f), sizes: props.phone!.sizes }))
        : []),
      { media: undefined, type: "image/avif", srcset: srcset(props.widths, "avif", props.density), sizes },
    ];
const img = {
  src: url(props.density ? props.widths[0]! : largest, "webp"),
  srcset: srcset(props.widths, "webp", props.density),
  sizes,
};

if (props.preload) {
  // the first screen's picture: the browser asks for it before it has read the page (AVIF only: a browser
  // that can not show it skips the hint and takes the WebP from the <picture>)
  useHead({
    link: sources.filter((s) => s.type === "image/avif").map((s, i) => ({
      rel: "preload",
      as: "image",
      type: s.type,
      imagesrcset: s.srcset,
      imagesizes: s.sizes,
      media: s.media ?? props.phone?.rest,
      fetchpriority: "high",
      key: `pic-${props.src}-${i}`,
    })),
  });
}
</script>

<template>
  <img
    v-if="single"
    :src="img.src"
    :srcset="img.srcset"
    :sizes="img.sizes"
    :alt="alt"
    :width="width"
    :height="height"
    :loading="loading"
    :decoding="decoding"
    :fetchpriority="fetchpriority"
  >
  <picture v-else>
    <source v-for="s in sources" :key="s.type + s.media" :media="s.media" :type="s.type" :srcset="s.srcset" :sizes="s.sizes">
    <img
      :src="img.src"
      :srcset="img.srcset"
      :sizes="img.sizes"
      :alt="alt"
      :width="width"
      :height="height"
      :loading="loading"
      :decoding="decoding"
      :fetchpriority="fetchpriority"
    >
  </picture>
</template>
