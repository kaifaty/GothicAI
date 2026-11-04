// The project's site: Nuxt 4 generating plain HTML and CSS (no client Vue), our one small script inlined.
// The look and its rules: README.md ("Design").
import { readFileSync } from "node:fs";
import { parse } from "smol-toml";
import { load } from "./lib/content";

const root = import.meta.dirname;
const settings = (parse(readFileSync(`${root}/content/home.toml`, "utf8")).site ?? {}) as { base?: string };
// GitHub Pages serves the site under /GothicAI/ (home.toml); `bun run dev` and NUXT_APP_BASE_URL=/ serve it at /
const dev = process.argv.includes("dev");
const baseURL = process.env.NUXT_APP_BASE_URL || (dev ? "/" : settings.base) || "/";

export default defineNuxtConfig({
  compatibilityDate: "2026-11-01",
  modules: ["@nuxt/fonts", "@nuxt/image", "./modules/site"],
  devtools: { enabled: false },
  telemetry: false,

  app: {
    baseURL,
    head: { viewport: "width=device-width, initial-scale=1, viewport-fit=cover" },
  },
  css: ["~/assets/style.css"],

  features: {
    noScripts: "production", // pages are plain HTML and CSS; app/assets/site.js is the only script
    inlineStyles: true,
  },
  // no client app, so no payload files (Nuxt warns that this is suboptimal: it is not, here); 404.html is
  // rendered as a page, not an empty shell
  experimental: { payloadExtraction: false, appManifest: false, prerenderErrorPages: [404] },

  nitro: {
    preset: "github_pages", // static files, 404.html and .nojekyll
    prerender: { crawlLinks: false, failOnError: true },
  },

  fonts: {
    // the faces style.css names, downloaded from Google Fonts when the site is built and served with it:
    // the reader's browser asks no font host. Preloaded per page by server/plugins/page.ts.
    defaults: {
      subsets: ["latin", "latin-ext", "cyrillic"],
      // metric-matched stand-ins while a face loads: the common ones only, to keep the inlined CSS small
      fallbacks: { serif: ["Georgia", "Noto Serif"], monospace: ["Menlo", "Courier New"] },
    },
    families: [
      { name: "Cormorant Garamond", provider: "google", weights: [500, 600], styles: ["normal"] },
      // italic only for a post's own emphasis (Google's API adds italic 500 and 600: declared, never loaded)
      { name: "Spectral", provider: "google", weights: [400, 500, 600], styles: ["normal", "italic"] },
      { name: "JetBrains Mono", provider: "google", weights: [400, 500], styles: ["normal"] },
    ],
    processCSSVariables: true,
  },

  image: {
    // the pictures are made smaller while the pages are prerendered (components/SitePicture.vue), read
    // where they lie: static/ as /static/, a post's folder as /media/<slug>/
    ipx: { fs: { dir: [root] } },
    alias: Object.fromEntries(load(root).posts.map((p) => [`/media/${p.slug}`, `/content/posts/${p.folder.split("/").pop()}`])),
  },
});
