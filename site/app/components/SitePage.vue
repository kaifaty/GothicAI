<!-- A whole page around its content: the head (title, description, link previews, the feed, icons), the bar,
<main> and the footer. `here` and `other` are this page and its other language under the base path;
`current` names the bar's link to mark ("devlog"); `image` is the link preview's picture. -->
<script setup lang="ts">
import { script } from "#site-content";

const props = withDefaults(
  defineProps<{
    lang: Lang;
    title: string;
    here: string;
    other: string;
    description?: string;
    cls?: string;
    current?: string;
    image?: string;
  }>(),
  { description: "", cls: "", current: "", image: "" },
);
const { base, absolute } = useLinks();
const ui = UI[props.lang];
const s = site.settings;
const home = site.home[props.lang];
const name = s.title ?? "Gothic AI";
const desc = props.description || home.description || "";
const brand = `${base}static/brand`;

// the valley at the top of the inner pages and under the footer (style.css: var(--valley-2)), as small as
// the screen allows; AVIF where the browser can choose by type
const $img = useImage();
const valley = (width: number, format: "avif" | "webp") =>
  $img("/static/art/valley-2.webp", { width, format, quality: format === "avif" ? 55 : 78 });
// (a phone sees it at 30% and less, under a mask: 640 px is enough)
const valleyCss =
  `:root{--valley-2:url("${valley(640, "webp")}")}` +
  `@media (min-width:48rem){:root{--valley-2:url("${valley(1672, "webp")}")}}` +
  `@supports (background-image:image-set(url("data:,") type("image/avif"))){` +
  `:root{--valley-2:image-set(url("${valley(640, "avif")}") type("image/avif"),url("${valley(640, "webp")}") type("image/webp"))}` +
  `@media (min-width:48rem){:root{--valley-2:image-set(url("${valley(1672, "avif")}") type("image/avif"),url("${valley(1672, "webp")}") type("image/webp"))}}}`;

useHead({
  htmlAttrs: { lang: props.lang },
  bodyAttrs: { class: props.cls || undefined },
  title: props.title === name ? props.title : `${props.title} | ${name}`,
  meta: [
    { name: "description", content: desc },
    { property: "og:type", content: props.cls === "article" ? "article" : "website" },
    { property: "og:site_name", content: name },
    { property: "og:url", content: absolute(props.here) },
    { property: "og:title", content: props.title },
    { property: "og:description", content: desc },
    { property: "og:image", content: absolute(props.image || `${brand}/og.jpg`) },
    { name: "twitter:card", content: "summary_large_image" },
    { name: "theme-color", content: "#14110D" },
  ],
  link: [
    { rel: "canonical", href: absolute(props.here) },
    { rel: "alternate", hreflang: props.lang, href: absolute(props.here) },
    { rel: "alternate", hreflang: ui.other_lang, href: absolute(props.other) },
    { rel: "alternate", type: "application/rss+xml", title: home.feed_title ?? "", href: `${base}${prefix(props.lang)}feed.xml` },
    { rel: "icon", href: `${brand}/favicon.ico`, sizes: "48x48" },
    { rel: "icon", href: `${brand}/favicon.svg`, type: "image/svg+xml" },
    { rel: "apple-touch-icon", href: `${brand}/apple-touch-icon.png` },
    { rel: "manifest", href: `${brand}/site.webmanifest` },
  ],
  style: [{ innerHTML: valleyCss, key: "valley-2" }],
  script: [
    {
      // hidden until they rise into view, unless site.js never runs
      innerHTML:
        'document.documentElement.classList.add("js");' +
        'setTimeout(function(){if(!window.gothicSite)document.documentElement.classList.remove("js")},2500)',
      tagPriority: "critical",
      key: "js-class",
    },
    { innerHTML: script, tagPosition: "bodyClose", key: "site-js" },
  ],
});
</script>

<template>
  <a class="skip" href="#main">{{ ui.skip }}</a>
  <SiteBar :lang="lang" :other="other" :current="current" />
  <main id="main">
    <slot />
  </main>
  <SiteFoot :lang="lang" :other="other" />
</template>
