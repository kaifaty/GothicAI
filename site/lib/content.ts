/* What the site is made of: the settings and the home page (content/home.toml), the devlog posts
(content/posts/<date>-<slug>/{en,ru}.md plus their pictures and clips), the standing pages
(content/pages/<slug>/{en,ru}.md: the privacy policy; title and summary, no date). Read once when the
site is built; nothing is fetched from the browser.

A post is Markdown with TOML between `+++` lines on top:

    +++
    title = "Three days in"
    summary = "One sentence for the list and the feed."
    date = 2026-10-12T18:00:00+03:00
    cover = "cover.jpg"          # optional, a picture next to the post
    youtube = ""                 # optional, a video id
    +++
    The text...
*/

import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { extname, join } from "node:path";
import MarkdownIt from "markdown-it";
import { parse as parseToml, TomlDate, TomlError } from "smol-toml";

export const LANGS = ["en", "ru"] as const;
export type Lang = (typeof LANGS)[number];
const MEDIA = new Set([".jpg", ".jpeg", ".png", ".webp", ".gif", ".mp4", ".webm", ".svg"]);

export class ContentError extends Error {}

/** A moment with the time zone it was written in: the date shown is the author's. */
export interface PostDate {
  iso: string; // as written, with its offset
  ms: number; // the instant
  offset: number; // minutes east of UTC
}

/** A post or a page in one language. */
export interface Version {
  title: string;
  summary: string;
  body: string; // Markdown
  cover: string;
  youtube: string;
}

export interface Post {
  slug: string;
  date: PostDate;
  folder: string;
  versions: Partial<Record<Lang, Version>>;
  media: string[]; // file names in the folder
}

/** A page outside the devlog, at /<slug>/ and /ru/<slug>/, linked from every page's footer. */
export interface Page {
  slug: string;
  versions: Partial<Record<Lang, Version>>;
}

export interface Site {
  root: string;
  settings: Record<string, any>;
  home: Record<Lang, Record<string, any>>;
  hero: Record<string, any>;
  posts: Post[]; // newest first
  pages: Page[];
}

export function splitFrontMatter(text: string, where: string): [Record<string, any>, string] {
  if (!text.startsWith("+++")) throw new ContentError(`${where}: no +++ front matter`);
  const rest = text.slice(3);
  const end = rest.indexOf("\n+++");
  if (end < 0) throw new ContentError(`${where}: the front matter is not closed with +++`);
  let meta: Record<string, any>;
  try {
    meta = parseToml(rest.slice(0, end));
  } catch (err) {
    if (err instanceof TomlError) throw new ContentError(`${where}: ${err.message}`);
    throw err;
  }
  return [meta, rest.slice(end + 4).replace(/^\n+/, "")];
}

function postDate(value: unknown, where: string): PostDate {
  if (value instanceof TomlDate && value.isDate()) {
    // a bare day: noon in UTC
    const iso = `${value.toISOString().slice(0, 10)}T12:00:00+00:00`;
    return { iso, ms: Date.parse(iso), offset: 0 };
  }
  if (!(value instanceof TomlDate) || !value.isDateTime() || value.isLocal()) {
    throw new ContentError(`${where}: date needs a time zone, like 2026-10-12T18:00:00+03:00`);
  }
  const iso = value.toISOString();
  const zone = iso.match(/(Z|([+-])(\d\d):(\d\d))$/)!;
  const offset = zone[1] === "Z" ? 0 : (zone[2] === "-" ? -1 : 1) * (Number(zone[3]) * 60 + Number(zone[4]));
  return { iso, ms: value.getTime(), offset };
}

function version(meta: Record<string, any>, body: string): Version {
  return {
    title: String(meta.title),
    summary: String(meta.summary),
    body,
    cover: String(meta.cover ?? ""),
    youtube: String(meta.youtube ?? ""),
  };
}

export function loadPost(folder: string): Post {
  const versions: Partial<Record<Lang, Version>> = {};
  const dates = new Map<number, PostDate>();
  for (const lang of LANGS) {
    const path = join(folder, `${lang}.md`);
    if (!existsSync(path)) continue;
    const [meta, body] = splitFrontMatter(readFileSync(path, "utf8"), path);
    for (const key of ["title", "summary", "date"]) {
      if (!(key in meta)) throw new ContentError(`${path}: no ${key}`);
    }
    const date = postDate(meta.date, path);
    dates.set(date.ms, date);
    versions[lang] = version(meta, body);
  }
  if (!versions.en) throw new ContentError(`${folder}: a post needs en.md (ru.md is optional)`);
  if (dates.size > 1) throw new ContentError(`${folder}: the languages have different dates`);
  const name = folder.split("/").pop()!;
  const dated = (name.slice(0, 10).match(/-/g) ?? []).length === 2 && name.slice(10, 11) === "-";
  const slug = dated ? name.slice(11) : name;
  const media = readdirSync(folder)
    .filter((f) => MEDIA.has(extname(f).toLowerCase()))
    .sort();
  for (const v of Object.values(versions)) {
    if (v.cover && !existsSync(join(folder, v.cover))) throw new ContentError(`${folder}: no cover ${v.cover}`);
  }
  return { slug, date: [...dates.values()][0]!, folder, versions, media };
}

export function loadPage(folder: string): Page {
  const versions: Partial<Record<Lang, Version>> = {};
  for (const lang of LANGS) {
    const path = join(folder, `${lang}.md`);
    if (!existsSync(path)) continue;
    const [meta, body] = splitFrontMatter(readFileSync(path, "utf8"), path);
    for (const key of ["title", "summary"]) {
      if (!(key in meta)) throw new ContentError(`${path}: no ${key}`);
    }
    versions[lang] = { ...version(meta, body), cover: "", youtube: "" };
  }
  if (!versions.en) throw new ContentError(`${folder}: a page needs en.md (ru.md is optional)`);
  return { slug: folder.split("/").pop()!, versions };
}

function folders(dir: string): string[] {
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .sort()
    .map((name) => join(dir, name))
    .filter((p) => statSync(p).isDirectory());
}

/** `root` is site/: content/home.toml, content/posts/, content/pages/. */
export function load(root: string): Site {
  const path = join(root, "content", "home.toml");
  let data: Record<string, any>;
  try {
    data = parseToml(readFileSync(path, "utf8"));
  } catch (err) {
    throw new ContentError(`${path}: ${(err as Error).message}`);
  }
  const missing = LANGS.filter((lang) => !(lang in data));
  if (missing.length) throw new ContentError(`${path}: no [${missing.join("], [")}]`);
  const posts = folders(join(root, "content", "posts")).map(loadPost);
  const slugs = posts.map((p) => p.slug);
  if (new Set(slugs).size !== slugs.length) {
    throw new ContentError(`two posts have the same slug: ${slugs.join(", ")}`);
  }
  posts.sort((a, b) => b.date.ms - a.date.ms); // stable: same moment keeps the folders' order
  const pages = folders(join(root, "content", "pages")).map(loadPage);
  const taken = new Set(["posts", "static", "media", "_ipx", "_fonts", "_nuxt", ...LANGS]);
  const clash = pages.find((p) => taken.has(p.slug));
  if (clash) throw new ContentError(`a page can not be called ${clash.slug}: the site uses that path`);
  return {
    root,
    settings: data.site ?? {},
    home: { en: data.en, ru: data.ru },
    hero: data.hero ?? {},
    posts,
    pages,
  };
}

// --- Markdown ---------------------------------------------------------------------------------------

const markdown = new MarkdownIt({ html: true, typographer: true });
const image = markdown.renderer.rules.image!;
// a picture in a post loads when it is scrolled near, and keeps its place while it does
markdown.renderer.rules.image = (tokens, idx, options, env: { sizes?: Record<string, [number, number]> }, self) => {
  const token = tokens[idx]!;
  const size = env?.sizes?.[token.attrGet("src") ?? ""];
  if (size) {
    token.attrSet("width", String(size[0]));
    token.attrSet("height", String(size[1]));
  }
  token.attrSet("loading", "lazy");
  token.attrSet("decoding", "async");
  return image(tokens, idx, options, env, self);
};

/** Markdown as HTML; `sizes` gives the pictures it names (by their link) their width and height. */
export function md(text: string, sizes: Record<string, [number, number]> = {}): string {
  return markdown.render(text, { sizes });
}

/** One line of Markdown (links, emphasis) without its paragraph. */
export function mdInline(text: string): string {
  return markdown.renderInline(text);
}

/** Relative pictures and links of a post point at its media folder under `base`. */
export function mediaLinks(html: string, base: string, slug: string): string {
  return html.replace(/\b(src|href|poster)="([^"]+)"/g, (whole, attr: string, url: string) =>
    /^([a-z]+:|\/|#)/.test(url) ? whole : `${attr}="${base}media/${slug}/${url}"`,
  );
}

// --- dates ------------------------------------------------------------------------------------------

const MONTHS: Record<Lang, string[]> = {
  en: "January February March April May June July August September October November December".split(" "),
  ru: "января февраля марта апреля мая июня июля августа сентября октября ноября декабря".split(" "),
};
const DAYS = "Sun Mon Tue Wed Thu Fri Sat".split(" ");
const MON = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split(" ");

/** The date's own calendar fields, in the zone it was written in. */
function local(d: PostDate): Date {
  return new Date(d.ms + d.offset * 60_000); // read with the UTC getters
}

export function dateText(d: PostDate, lang: Lang): string {
  const t = local(d);
  const month = MONTHS[lang][t.getUTCMonth()];
  return lang === "en"
    ? `${month} ${t.getUTCDate()}, ${t.getUTCFullYear()}`
    : `${t.getUTCDate()} ${month} ${t.getUTCFullYear()}`;
}

/** The day as YYYY-MM-DD, for <time datetime>. */
export function dateDay(d: PostDate): string {
  return local(d).toISOString().slice(0, 10);
}

/** RFC 2822, as RSS wants it: "Tue, 13 Oct 2026 21:58:47 +0300". */
export function dateRfc822(d: PostDate): string {
  const t = local(d);
  const pad = (n: number) => String(n).padStart(2, "0");
  const sign = d.offset < 0 ? "-" : "+";
  const off = Math.abs(d.offset);
  return (
    `${DAYS[t.getUTCDay()]}, ${pad(t.getUTCDate())} ${MON[t.getUTCMonth()]} ${t.getUTCFullYear()} ` +
    `${pad(t.getUTCHours())}:${pad(t.getUTCMinutes())}:${pad(t.getUTCSeconds())} ${sign}${pad(Math.floor(off / 60))}${pad(off % 60)}`
  );
}
