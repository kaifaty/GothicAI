/* The site's content at build time: reads content/ once (lib/content.ts), hands it to the pages as the
module `#site-content`, publishes static/ and the posts' pictures, lists every page to prerender, writes
the RSS feeds and minifies our one script. In development a change under content/ restarts the server. */

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { addServerHandler, addTemplate, addTypeTemplate, defineNuxtModule } from "@nuxt/kit";
import { transformSync } from "esbuild";
import sharp from "sharp";
import {
  dateDay,
  dateRfc822,
  dateText,
  LANGS,
  load,
  md,
  mdInline,
  mediaLinks,
  type Lang,
  type Site,
} from "../lib/content";

const escape = (s: string) =>
  s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#x27;");
const prefix = (lang: Lang) => (lang === "en" ? "" : `${lang}/`);

/** What the pages get: the content with its Markdown already turned into HTML under `base`. */
function pageData(site: Site, base: string, sizes: Sizes) {
  return {
    settings: site.settings,
    hero: site.hero,
    home: Object.fromEntries(
      LANGS.map((lang) => {
        const home = site.home[lang];
        return [lang, { ...home, credit: home.credit ? mdInline(home.credit) : "" }];
      }),
    ) as Record<Lang, Record<string, any>>,
    posts: site.posts.map((post) => ({
      slug: post.slug,
      day: dateDay(post.date),
      date: Object.fromEntries(LANGS.map((lang) => [lang, dateText(post.date, lang)])) as Record<Lang, string>,
      versions: Object.fromEntries(
        Object.entries(post.versions).map(([lang, v]) => [
          lang,
          { title: v.title, summary: v.summary, cover: v.cover, youtube: v.youtube, html: mediaLinks(md(v.body, sizes[post.slug]), base, post.slug) },
        ]),
      ) as Partial<Record<Lang, { title: string; summary: string; cover: string; youtube: string; html: string }>>,
    })),
    pages: site.pages.map((page) => ({
      slug: page.slug,
      versions: Object.fromEntries(
        Object.entries(page.versions).map(([lang, v]) => [lang, { title: v.title, summary: v.summary, html: md(v.body) }]),
      ) as Partial<Record<Lang, { title: string; summary: string; html: string }>>,
    })),
  };
}
export type SiteData = ReturnType<typeof pageData>;

/** Per post, the width and height of each of its pictures. */
type Sizes = Record<string, Record<string, [number, number]>>;
async function pictureSizes(site: Site): Promise<Sizes> {
  const out: Sizes = {};
  for (const post of site.posts) {
    out[post.slug] = {};
    for (const name of post.media.filter((f) => /\.(jpe?g|png|webp|gif)$/i.test(f))) {
      const { width, height } = await sharp(join(post.folder, name)).metadata();
      if (width && height) out[post.slug]![name] = [width, height];
    }
  }
  return out;
}

/** The RSS feed of one language, every link absolute. */
export function feed(site: Site, lang: Lang, sizes: Sizes = {}): string {
  const s = site.settings;
  const h = site.home[lang];
  const url = String(s.url ?? "").replace(/\/+$/, "");
  const items = site.posts.flatMap((post) => {
    const v = post.versions[lang];
    if (!v) return [];
    const link = `${url}/${prefix(lang)}posts/${post.slug}/`;
    const html = mediaLinks(md(v.body, sizes[post.slug]), `${url}/`, post.slug);
    return [
      `<item><title>${escape(v.title)}</title><link>${link}</link>` +
        `<guid isPermaLink="true">${link}</guid>` +
        `<pubDate>${dateRfc822(post.date)}</pubDate>` +
        `<description>${escape(v.summary)}</description>` +
        `<content:encoded>${escape(html)}</content:encoded></item>`,
    ];
  });
  return (
    '<?xml version="1.0" encoding="utf-8"?>\n' +
    '<rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/" ' +
    'xmlns:atom="http://www.w3.org/2005/Atom"><channel>' +
    `<title>${escape(h.feed_title ?? s.title ?? "")}</title>` +
    `<link>${url}/${prefix(lang)}</link>` +
    `<atom:link href="${url}/${prefix(lang)}feed.xml" rel="self" type="application/rss+xml"/>` +
    `<description>${escape(h.description ?? "")}</description><language>${lang}</language>` +
    `${items.join("")}</channel></rss>\n`
  );
}

/** Every address of the site, under the base path. */
export function routes(site: Site): string[] {
  const out: string[] = [];
  for (const lang of LANGS) {
    const p = `/${prefix(lang)}`;
    out.push(p, `${p}posts/`, `${p}feed.xml`);
    for (const post of site.posts) if (post.versions[lang]) out.push(`${p}posts/${post.slug}/`);
    for (const page of site.pages) if (page.versions[lang]) out.push(`${p}${page.slug}/`);
  }
  return [...out, "/404.html"];
}

export default defineNuxtModule({
  meta: { name: "gothic-site" },
  async setup(_, nuxt) {
    const root = nuxt.options.rootDir;
    const base = nuxt.options.app.baseURL.endsWith("/") ? nuxt.options.app.baseURL : `${nuxt.options.app.baseURL}/`;
    const site = load(root);
    const sizes = await pictureSizes(site);
    const data = pageData(site, base, sizes);
    const script = transformSync(readFileSync(join(root, "app/assets/site.js"), "utf8"), { minify: true, target: "es2017" }).code.trim();

    const content = addTemplate({
      filename: "site-content.mjs",
      getContents: () => `export default ${JSON.stringify(data)};\nexport const script = ${JSON.stringify(script)};\n`,
    });
    nuxt.options.alias["#site-content"] = content.dst;
    addTypeTemplate({
      filename: "types/site-content.d.ts",
      getContents: () =>
        `declare module "#site-content" {\n  const data: import(${JSON.stringify(join(root, "modules/site"))}).SiteData;\n  export default data;\n  export const script: string;\n}\n`,
    });

    nuxt.hook("nitro:config", (config) => {
      config.publicAssets ||= [];
      config.publicAssets.push({ dir: join(root, "static"), baseURL: "/static", maxAge: 0 });
      for (const post of site.posts) config.publicAssets.push({ dir: post.folder, baseURL: `/media/${post.slug}`, maxAge: 0 });
      config.ignore = [...(config.ignore ?? []), "**/*.md"]; // the posts' text is in the pages, not beside the pictures
      config.virtual ||= {};
      config.virtual["#site-feeds"] = () =>
        `export default ${JSON.stringify(Object.fromEntries(LANGS.map((lang) => [lang, feed(site, lang, sizes)])))};`;
      config.prerender ||= {};
      config.prerender.routes = [...(config.prerender.routes ?? []), ...routes(site)];
    });
    // the site has no client-side app: no SPA fallback page next to 404.html
    nuxt.hook("nitro:init", (nitro) => {
      nitro.hooks.hook("prerender:generate", (route) => {
        if (route.route === "/200.html") route.skip = true;
      });
    });
    for (const lang of LANGS) {
      addServerHandler({ route: `/${prefix(lang)}feed.xml`, handler: join(root, `server/feeds/${lang}.ts`) });
    }

    // a new post or a changed text restarts the dev server with the content read again
    nuxt.options.watch.push(join(root, "content"), join(root, "lib"), join(root, "app/assets/site.js"));
  },
});
