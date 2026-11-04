import { existsSync, mkdtempSync, readdirSync, readFileSync, statSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, relative } from "node:path";
import { XMLParser, XMLValidator } from "fast-xml-parser";
import { describe, expect, inject, test } from "vitest";
import { ContentError, load, loadPost } from "../lib/content";

const SITE = join(import.meta.dirname, "..");
const out = inject("out");
const read = (rel: string) => readFileSync(join(out, rel), "utf8");
const pages = (dir = out): string[] =>
  readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) return pages(path);
    return /\.(html|css|xml)$/.test(name) ? [relative(out, path)] : [];
  });

describe("the generated site", () => {
  const site = load(SITE);

  test("every page of both languages is there", () => {
    for (const prefix of ["", "ru/"]) {
      expect(existsSync(join(out, prefix, "index.html"))).toBe(true);
      expect(existsSync(join(out, prefix, "posts/index.html"))).toBe(true);
      for (const post of site.posts) expect(existsSync(join(out, prefix, `posts/${post.slug}/index.html`))).toBe(true);
    }
    expect(existsSync(join(out, "404.html")) && existsSync(join(out, ".nojekyll"))).toBe(true);
    expect(existsSync(join(out, "200.html"))).toBe(false);
    for (const post of site.posts) {
      for (const f of post.media) expect(existsSync(join(out, "media", post.slug, f)), `${post.slug}/${f}`).toBe(true);
      expect(existsSync(join(out, "media", post.slug, "en.md"))).toBe(false);
    }
  });

  test("links live under the base path", () => {
    const html = read("ru/posts/shorts-by-the-agent/index.html");
    expect(html).toMatch(/<style[^>]*>[^<]*--night:/); // the CSS is inlined
    expect(html).not.toContain('rel="stylesheet"');
    expect(html).toContain('src="/GothicAI/media/shorts-by-the-agent/sheet-ru.jpg"'); // relative in the Markdown
    expect(html).toContain('href="/GothicAI/posts/shorts-by-the-agent/"'); // the same post in English
    expect(html).toMatch(/<html\s+lang="ru">/);
  });

  test("the feeds are valid RSS with absolute links", () => {
    for (const prefix of ["", "ru/"]) {
      const xml = read(`${prefix}feed.xml`);
      expect(XMLValidator.validate(xml)).toBe(true);
      const items = new XMLParser().parse(xml).rss.channel.item;
      expect(items.length).toBeGreaterThan(0);
      for (const item of items) {
        expect(item.link).toMatch(/^https:\/\/kaifaty\.github\.io\/GothicAI\//);
        expect(item.pubDate).toBeTruthy();
      }
    }
  });

  test("the home page shows the conversation and the short, which loads nothing before it is near", () => {
    const html = read("index.html");
    expect(html.split('class="line ').length - 1).toBeGreaterThanOrEqual(2);
    expect(html).toContain("by-hero");
    expect(html).toContain("by-npc");
    expect(html).toContain('<source data-src="/GothicAI/static/hero/wrath-en.mp4"');
    expect(html).toContain('preload="none"');
    expect(html).not.toMatch(/<(video|source)[^>]* src=/);
    const ru = read("ru/index.html");
    expect(ru).toContain("wrath-ru.mp4");
    expect(ru).toContain("Ксардас");
  });

  test("the home page carries the claim, the steps and the credit", () => {
    const html = read("index.html");
    expect(html).toMatch(/data-words(="")?>Nobody wrote Xardas/);
    expect(html.split('class="step-n"').length - 1).toBe(5);
    expect(html).toContain("<h3>Recognition</h3>");
    expect(html).toContain('href="https://github.com/Try/OpenGothic">OpenGothic</a>'); // the footer's credit
    expect(html).toContain('content="https://kaifaty.github.io/GothicAI/static/brand/og.jpg"');
    const post = read("posts/blade-hits/index.html");
    expect(post).toContain('aria-current="page"'); // the bar marks the devlog
    expect(post).toContain('class="older"');
    expect(post).toContain('content="https://kaifaty.github.io/GothicAI/media/blade-hits/cover.jpg"');
  });

  test("the illustrations are on the home page, made smaller, and named as such", () => {
    const html = read("index.html");
    expect(html).toMatch(/srcset="\/GothicAI\/_ipx\/w_800&amp;f_avif[^"]*\/static\/art\/valley\.webp 800w"/); // the first screen
    expect(html).toMatch(/<link rel="preload" as="image" type="image\/avif"[^>]*valley\.webp[^>]*fetchpriority="high"/);
    expect(html.split("<figure data-reveal").length - 1).toBe(4);
    expect(html).toContain("<figcaption>Miner</figcaption>");
    expect(html).toContain("Illustrations drawn for the site");
    expect(html).toContain("the frames of the game are real takes");
    for (const name of ["valley", "valley-2", "mage", "miner", "guard", "novice", "hunter", "parchment"]) {
      expect(existsSync(join(SITE, "static/art", `${name}.webp`))).toBe(true);
    }
    for (const img of html.match(/<img [^>]*>/g) ?? []) expect(img, img).toMatch(/ width="\d+" height="\d+"/);
  });

  test("a YouTube short loads nothing from YouTube until it is clicked", () => {
    const html = read("posts/xardas-wrath/index.html");
    const short = html.match(/<div class="short">.*?<\/button><\/div>/s)?.[0] ?? "";
    expect(short).toContain('data-id="OMR5Bi_KGEo"');
    expect(short).toContain("<button");
    expect(short).not.toMatch(/youtube|<video|<iframe/); // the iframe comes from the script on click
  });

  test("the privacy page is linked from every page", () => {
    for (const prefix of ["", "ru/"]) expect(existsSync(join(out, prefix, "privacy/index.html"))).toBe(true);
    expect(read("index.html")).toContain('href="/GothicAI/privacy/">Privacy</a>');
    expect(read("ru/index.html")).toContain('href="/GothicAI/ru/privacy/">');
    expect(read("privacy/index.html")).toContain('href="https://myaccount.google.com/permissions"');
  });

  test("no script but ours: the head's safety net and the page script", () => {
    for (const rel of pages().filter((p) => p.endsWith(".html"))) {
      const tags = read(rel).match(/<script\b[^>]*>/g) ?? [];
      expect(tags, rel).toEqual(['<script data-hid="js-class">', '<script data-hid="site-js">']);
    }
    expect(readdirSync(join(out, "_nuxt")).filter((f) => f.endsWith(".js"))).toEqual([]);
  });

  test("no font host: the fonts are the site's own", () => {
    for (const rel of pages()) expect(read(rel), rel).not.toMatch(/fonts\.(googleapis|gstatic)\.com/);
    expect(read("index.html")).toMatch(/@font-face\{font-family:"?Cormorant Garamond"?;[^}]*url\(\/GothicAI\/_nuxt\/fonts\//);
    expect(read("index.html")).toMatch(/<link rel="preload" as="font" type="font\/woff2" href="\/GothicAI\/_nuxt\/fonts\//);
  });
});

describe("the content", () => {
  test("a post needs its front matter", () => {
    const post = join(mkdtempSync(join(tmpdir(), "site-")), "2026-10-13-oops");
    mkdirSync(post);
    writeFileSync(join(post, "en.md"), '+++\ntitle = "x"\n+++\ntext');
    expect(() => loadPost(post)).toThrow(/summary/);
    writeFileSync(join(post, "en.md"), '+++\ntitle = "x"\nsummary = "y"\ndate = 2026-10-13T10:00:00\n+++\n');
    expect(() => loadPost(post)).toThrow(ContentError);
    expect(() => loadPost(post)).toThrow(/time zone/);
    writeFileSync(join(post, "en.md"), '+++\ntitle = "x"\nsummary = "y"\ndate = 2026-10-13T10:00:00+03:00\n+++\nok');
    expect(loadPost(post).slug).toBe("oops");
    writeFileSync(join(post, "en.md"), '+++\ntitle = "x"\nsummary = "y"\ndate = 2026-10-13T10:00:00+03:00\ncover = "c.jpg"\n+++\n');
    expect(() => loadPost(post)).toThrow(/no cover c\.jpg/);
  });
});
