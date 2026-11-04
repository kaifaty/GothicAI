"""The site as files: the home page and the devlog in English (/) and Russian (/ru/), a page per post, the
standing pages (the privacy policy), an RSS feed per language, the static files and the posts' pictures.
Every link starts with the base path (/GothicAI/ on GitHub Pages, / for a local preview). The look is in
static/style.css and its rules in README.md ("Design")."""

from __future__ import annotations

import datetime as dt
import email.utils
import re
import shutil
from html import escape
from pathlib import Path

import markdown

from .content import LANGS, Page, Post, Site

FONTS = (
    "https://fonts.googleapis.com/css2?family=Cormorant+Garamond:wght@500;600"
    "&family=JetBrains+Mono:wght@400;500&family=Spectral:ital,wght@0,400;0,500;0,600;1,400&display=swap"
)
MONTHS = {
    "en": "January February March April May June July August September October November December".split(),
    "ru": "января февраля марта апреля мая июня июля августа сентября октября ноября декабря".split(),
}
ROMAN = "I II III IV V VI VII VIII IX X XI XII".split()
PEOPLE = [
    "miner",
    "guard",
    "novice",
    "hunter",
]  # static/art/<name>.webp, in the order of `people` in home.toml
UI = {
    "en": {
        "devlog": "Devlog", "source": "Source", "other": "Русский", "other_lang": "ru", "home": "Gothic AI",
        "all_posts": "All posts", "feed": "RSS feed", "play": "Play the short", "back": "Back to the devlog",
        "telegram": "Telegram", "not_found": "There is no such page.", "to_home": "Go to the home page",
        "skip": "Skip to content", "menu": "Main", "short": "Short", "newer": "Newer", "older": "Older",
        "empty": "Nothing here yet.", "pause": "Pause",
    },
    "ru": {
        "devlog": "Дневник", "source": "Код", "other": "English", "other_lang": "en", "home": "Gothic AI",
        "all_posts": "Все записи", "feed": "RSS", "play": "Смотреть шортс", "back": "Ко всем записям",
        "telegram": "Telegram", "not_found": "Такой страницы нет.", "to_home": "На главную",
        "skip": "К содержанию", "menu": "Главное", "short": "Шортс", "newer": "Позже", "older": "Раньше",
        "empty": "Пока пусто.", "pause": "Пауза",
    },
}  # fmt: skip


def prefix(lang: str) -> str:
    return "" if lang == "en" else f"{lang}/"


def date_text(d: dt.datetime, lang: str) -> str:
    return (
        f"{MONTHS[lang][d.month - 1]} {d.day}, {d.year}"
        if lang == "en"
        else f"{d.day} {MONTHS[lang][d.month - 1]} {d.year}"
    )


def md(text: str) -> str:
    return markdown.markdown(text, extensions=["extra", "sane_lists", "smarty"], output_format="html")


def md_inline(text: str) -> str:
    """One line of Markdown (links, emphasis) without its paragraph."""
    return re.sub(r"^<p>|</p>$", "", md(text).strip())


def media_links(html: str, base: str, post: Post) -> str:
    """Relative pictures and links of a post point at its media folder."""

    def fix(m: re.Match[str]) -> str:
        attr, url = m.group(1), m.group(2)
        if re.match(r"^([a-z]+:|/|#)", url):
            return m.group(0)
        return f'{attr}="{base}media/{post.slug}/{url}"'

    return re.sub(r'\b(src|href|poster)="([^"]+)"', fix, html)


class Builder:
    def __init__(self, site: Site, out: Path, base: str | None = None) -> None:
        self.site = site
        self.out = out
        self.base = base if base is not None else site.base
        if not self.base.endswith("/"):
            self.base += "/"

    def build(self) -> list[Path]:
        if self.out.exists():
            shutil.rmtree(self.out)
        self.out.mkdir(parents=True)
        written: list[Path] = []
        shutil.copytree(self.site.root / "static", self.out / "static")
        for post in self.site.posts:
            folder = self.out / "media" / post.slug
            folder.mkdir(parents=True)
            for f in post.media:
                shutil.copy(f, folder / f.name)
        for lang in LANGS:
            written.append(self._write(f"{prefix(lang)}index.html", self.home(lang)))
            written.append(self._write(f"{prefix(lang)}posts/index.html", self.devlog(lang)))
            written.append(self._write(f"{prefix(lang)}feed.xml", self.feed(lang)))
            for post in self.site.posts:
                if post.get(lang) is not None:
                    written.append(
                        self._write(f"{prefix(lang)}posts/{post.slug}/index.html", self.post(post, lang))
                    )
            for page in self.site.pages:
                if page.get(lang) is not None:
                    rel = f"{prefix(lang)}{page.slug}/index.html"
                    written.append(self._write(rel, self.standing(page, lang)))
        written.append(self._write("404.html", self.not_found()))
        (self.out / ".nojekyll").write_text("")
        return written

    def _write(self, rel: str, text: str) -> Path:
        path = self.out / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    # --- pieces ---------------------------------------------------------------------------------------

    def url(self, lang: str, rel: str = "") -> str:
        return f"{self.base}{prefix(lang)}{rel}"

    def absolute(self, url: str) -> str:
        """A link under the base path as the published address (for search engines and link previews)."""
        site_url = str(self.site.settings.get("url", "")).rstrip("/")
        return f"{site_url}/{url[len(self.base) :]}" if url.startswith(self.base) else url

    def page(
        self,
        lang: str,
        title: str,
        body: str,
        *,
        here: str,
        other: str,
        description: str = "",
        cls: str = "",
        current: str = "",
        image: str = "",
    ) -> str:
        """A whole page around `body`. `here` and `other` are this page and its other language under the base
        path; `current` names the bar's link to mark ("devlog"); `image` is the link preview's picture."""
        ui, s = UI[lang], self.site.settings
        home = self.site.home[lang]
        desc = escape(description or home.get("description", ""))
        full_title = escape(title if title == s.get("title") else f"{title} | {s.get('title', 'Gothic AI')}")
        at = ' aria-current="page"' if current == "devlog" else ""
        links = [f'<a href="{self.url(lang, "posts/")}"{at}>{ui["devlog"]}</a>']
        if s.get("telegram"):
            links.append(f'<a href="{escape(s["telegram"])}">{ui["telegram"]}</a>')
        if s.get("repo"):
            links.append(f'<a href="{escape(s["repo"])}">{ui["source"]}</a>')
        links.append(
            f'<a href="{other}" hreflang="{ui["other_lang"]}" lang="{ui["other_lang"]}">{ui["other"]}</a>'
        )
        image = image or f"{self.base}static/brand/og.jpg"
        feed = (
            f'<link rel="alternate" type="application/rss+xml" title="{escape(home.get("feed_title", ""))}"'
        )
        foot = [f'<a href="{self.url(lang, "feed.xml")}">{ui["feed"]}</a>']
        if s.get("repo"):
            foot.append(f'<a href="{escape(s["repo"])}">GitHub</a>')
        for p in self.site.pages:
            at = lang if p.get(lang) else "en"
            foot.append(f'<a href="{self.url(at, f"{p.slug}/")}">{escape(p.versions[at].title)}</a>')
        foot.append(
            f'<a href="{other}" hreflang="{ui["other_lang"]}" lang="{ui["other_lang"]}">{ui["other"]}</a>'
        )
        credit = f'<p class="credit">{md_inline(home["credit"])}</p>' if home.get("credit") else ""
        brand = f"{self.base}static/brand"
        name = escape(s.get("title", "Gothic AI"))
        mark = (  # on a phone the bar shows the seal alone
            f'<picture><source media="(max-width: 30rem)" srcset="{brand}/favicon.svg">'
            f'<img src="{brand}/lockup.svg" width="127" height="32" alt="{name}"></picture>'
        )
        return f"""<!doctype html>
<html lang="{lang}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>{full_title}</title>
<meta name="description" content="{desc}">
<link rel="canonical" href="{self.absolute(here)}">
<link rel="alternate" hreflang="{lang}" href="{self.absolute(here)}">
<link rel="alternate" hreflang="{ui["other_lang"]}" href="{self.absolute(other)}">
<meta property="og:type" content="{"article" if cls == "article" else "website"}">
<meta property="og:site_name" content="{escape(s.get("title", "Gothic AI"))}">
<meta property="og:url" content="{self.absolute(here)}">
<meta property="og:title" content="{escape(title)}">
<meta property="og:description" content="{desc}">
<meta property="og:image" content="{escape(self.absolute(image))}">
<meta name="twitter:card" content="summary_large_image">
{feed} href="{self.url(lang, "feed.xml")}">
<script>
document.documentElement.classList.add("js");  // hidden until they rise into view, unless site.js never runs
setTimeout(function () {{ if (!window.gothicSite) document.documentElement.classList.remove("js"); }}, 2500);
</script>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="{FONTS}">
<link rel="stylesheet" href="{self.base}static/style.css">
<link rel="icon" href="{brand}/favicon.ico" sizes="48x48">
<link rel="icon" href="{brand}/favicon.svg" type="image/svg+xml">
<link rel="apple-touch-icon" href="{brand}/apple-touch-icon.png">
<link rel="manifest" href="{brand}/site.webmanifest">
<meta name="theme-color" content="#14110D">
</head>
<body class="{cls}">
<a class="skip" href="#main">{ui["skip"]}</a>
<header class="bar">
  <div class="bar-in">
    <a class="mark" href="{self.url(lang)}">{mark}</a>
    <nav aria-label="{ui["menu"]}">{"".join(links)}</nav>
  </div>
</header>
<main id="main">
{body}
</main>
<footer class="foot">
  <div class="foot-in">
    <img class="foot-seal" src="{brand}/favicon.svg" width="40" height="40" alt="">
    <p class="foot-status">{escape(home.get("status", ""))}</p>
    <nav class="foot-links">{"".join(foot)}</nav>
    {credit}
  </div>
</footer>
<script src="{self.base}static/site.js" defer></script>
</body>
</html>
"""

    def player(self, youtube: str, poster: str, loop: str, label: str, alt: str) -> str:
        """The short: a YouTube video behind a click (nothing loads from YouTube before it), else a muted loop
        that plays unless the reader asked for less motion, else the still. Behind it the same still, blurred,
        lights the page around the frame."""
        img = f'<img src="{poster}" alt="{escape(alt)}" width="540" height="960">' if poster else ""
        glow = f'<img class="glow" src="{poster}" alt="" aria-hidden="true">' if poster else ""
        if youtube:
            attrs = f'class="yt" data-id="{escape(youtube)}" aria-label="{escape(label)}"'
            play = '<span class="play" aria-hidden="true"></span>'
            return f'<div class="short">{glow}<button type="button" {attrs}>{img}{play}</button></div>'
        if loop:
            return (
                f'<div class="short">{glow}<video class="loop" muted loop playsinline preload="metadata" '
                f'poster="{poster}" aria-label="{escape(alt)}" width="540" height="960">'
                f'<source src="{loop}" type="video/mp4"></video></div>'
            )
        return f'<div class="short">{glow}{img}</div>' if img else ""

    def art(self, name: str) -> str:
        """An illustration from static/art/ (ours, drawn for the site, not a frame of the game), or ""."""
        found = self.site.root / "static" / "art" / f"{name}.webp"
        return f"{self.base}static/art/{name}.webp" if found.is_file() else ""

    def people(self, h: dict) -> str:
        """The people of the colony: a row of portraits with their names, marked as illustrations."""
        figures = []
        for i, name in enumerate(h.get("people", [])):
            src = self.art(PEOPLE[i % len(PEOPLE)])
            if src:
                figures.append(
                    f'<figure data-reveal><img src="{src}" alt="" width="768" height="1024" loading="lazy" '
                    f'decoding="async"><figcaption>{escape(name)}</figcaption></figure>'
                )
        if not figures:
            return ""
        note = f'<p class="art-note">{escape(h["art_note"])}</p>' if h.get("art_note") else ""
        return f"""
<section class="people">
  {self.head(escape(h.get("people_label", "")), escape(h.get("people_title", "")))}
  <div class="people-row">{"".join(figures)}</div>
  {note}
</section>"""

    def head(self, eyebrow: str, title: str, level: int = 2, more: str = "") -> str:
        """A section's heading: a small label over the title, and a link on the right."""
        label = f'<p class="eyebrow">{eyebrow}</p>' if eyebrow else ""
        return f'<header class="section-head"><div>{label}<h{level}>{title}</h{level}></div>{more}</header>'

    # --- pages ----------------------------------------------------------------------------------------

    def home(self, lang: str) -> str:
        h, hero, ui = self.site.home[lang], self.site.hero, UI[lang]
        lines = []
        for i, line in enumerate(h.get("dialogue", [])):
            who = "by-hero" if line.get("hero") else "by-npc"
            last = " last" if i == len(h["dialogue"]) - 1 else ""
            lines.append(
                f'<p class="line {who}{last}" data-reveal><span class="who">{escape(line["name"])}</span>'
                f'<span class="said">{escape(line["text"])}</span></p>'
            )
        poster = f"{self.base}static/{hero['poster']}" if hero.get("poster") else ""
        loop = f"{self.base}static/{hero[f'loop_{lang}']}" if hero.get(f"loop_{lang}") else ""
        player = self.player(hero.get("youtube", ""), poster, loop, ui["play"], h.get("video_alt", ""))
        features = "".join(
            f'<li data-reveal><span class="num">{ROMAN[i % len(ROMAN)]}</span>'
            f"<h3>{escape(f['title'])}</h3><p>{escape(f['text'])}</p></li>"
            for i, f in enumerate(h.get("features", []))
        )
        steps = []
        for i, step in enumerate(h.get("how_steps", [])):
            label, text = (step.get("label", ""), step["text"]) if isinstance(step, dict) else ("", step)
            title = f"<h3>{escape(label)}</h3>" if label else ""
            steps.append(f'<li><span class="step-n">{i + 1:02d}</span>{title}<p>{escape(text)}</p></li>')
        actions = [f'<a class="button" href="{self.url(lang, "posts/")}">{escape(h["read_devlog"])}</a>']
        if self.site.settings.get("repo"):
            repo = escape(self.site.settings["repo"])
            actions.append(f'<a class="arrow" href="{repo}">{escape(h["see_code"])}</a>')
        valley, mage = self.art("valley"), self.art("mage")
        # the first screen is the valley under the Barrier; the old mage looks on beside the claim
        backdrop = (
            f'<div class="backdrop" aria-hidden="true"><img src="{valley}" alt="" width="1672" height="941" '
            f'fetchpriority="high"></div>'
            if valley
            else ""
        )
        portrait = (
            f'<img class="portrait" src="{mage}" alt="" width="768" height="1024" loading="lazy">'
            if mage
            else ""
        )
        claim = (
            f'<section class="claim"><div class="claim-in">'
            f'<p class="claim-text" data-words>{escape(h["claim"])}</p>{portrait}</div></section>'
            if h.get("claim")
            else ""
        )
        all_posts = f'<a class="arrow" href="{self.url(lang, "posts/")}">{ui["all_posts"]}</a>'
        body = f"""
<section class="hero">
  {backdrop}
  <div class="hero-in">
    <div class="pitch">
      <p class="eyebrow">{escape(h.get("eyebrow", ""))}</p>
      <h1>{escape(h["headline"])}</h1>
      <p class="lede">{escape(h["lede"])}</p>
      <p class="actions">{"".join(actions)}</p>
      <p class="status">{escape(h.get("status", ""))}</p>
    </div>
  </div>
</section>
<section class="scene">
  <div class="scene-in">
    <div class="scene-head">
      {player}
      <p class="eyebrow">{escape(h.get("dialogue_label", ""))}</p>
      <p class="note">{escape(h.get("dialogue_note", ""))}</p>
    </div>
    <div class="dialogue" aria-label="{escape(h.get("dialogue_label", ""))}">
      {"".join(lines)}
    </div>
  </div>
</section>
{claim}
{self.people(h)}
<section class="works">
  {self.head("", escape(h["features_title"]))}
  <ol class="features">{features}</ol>
</section>
<section class="how">
  <div class="diary">
    {self.head("", escape(h["how_title"]))}
    <p class="how-text">{escape(h["how_text"])}</p>
    <ol class="flow" data-reveal>{"".join(steps)}</ol>
  </div>
</section>
<section class="log">
  {self.head("", escape(h["devlog_title"]), more=all_posts)}
  {self.cards(lang, limit=4, level=3, cls="cards shelf")}
</section>
"""
        other = self.url(UI[lang]["other_lang"])
        title = self.site.settings.get("title", "Gothic AI")
        return self.page(lang, title, body, here=self.url(lang), other=other, cls="home")

    def cards(self, lang: str, limit: int | None = None, level: int = 2, cls: str = "cards") -> str:
        """The posts as cards: the cover, the date, the title and the summary, newest first."""
        ui, items = UI[lang], []
        for post in [p for p in self.site.posts if p.get(lang)][:limit]:
            v = post.get(lang)
            assert v is not None
            if v.cover:
                src = f"{self.base}media/{post.slug}/{escape(v.cover)}"
                pic = f'<img src="{src}" alt="" width="540" height="960" loading="lazy" decoding="async">'
            else:
                pic = f'<img class="seal" src="{self.base}static/brand/favicon.svg" alt="">'
            badge = f'<span class="badge">{ui["short"]}</span>' if v.youtube else ""
            items.append(
                f'<li data-reveal><a class="card" href="{self.url(lang, f"posts/{post.slug}/")}">'
                f'<span class="pic">{pic}{badge}</span>'
                f'<time datetime="{post.date.date().isoformat()}">{date_text(post.date, lang)}</time>'
                f'<h{level}>{escape(v.title)}</h{level}><span class="sum">{escape(v.summary)}</span></a></li>'
            )
        if not items:
            return f'<p class="empty">{ui["empty"]}</p>'
        return f'<ol class="{cls}" reversed>{"".join(items)}</ol>'

    def devlog(self, lang: str) -> str:
        h, ui = self.site.home[lang], UI[lang]
        feed = f'<a class="arrow" href="{self.url(lang, "feed.xml")}">{ui["feed"]}</a>'
        lede = f'<p class="lede">{escape(h["devlog_lede"])}</p>' if h.get("devlog_lede") else ""
        body = f"""
<section class="devlog">
  {self.head("", escape(h["devlog_title"]), level=1, more=feed)}
  {lede}
  {self.cards(lang)}
</section>
"""
        here, other = self.url(lang, "posts/"), self.url(ui["other_lang"], "posts/")
        return self.page(lang, h["devlog_title"], body, here=here, other=other, current="devlog")

    def post(self, post: Post, lang: str) -> str:
        v = post.get(lang)
        assert v is not None
        ui = UI[lang]
        poster = f"{self.base}media/{post.slug}/{v.cover}" if v.cover else ""
        cover = self.player(v.youtube, poster, "", ui["play"], v.title) if v.youtube or v.cover else ""
        aside = f'<aside class="post-short">{cover}</aside>' if cover else ""
        body_html = media_links(md(v.body), self.base, post)
        other_lang = ui["other_lang"]
        other = (
            self.url(other_lang, f"posts/{post.slug}/")
            if post.get(other_lang)
            else self.url(other_lang, "posts/")
        )
        when = f'<time datetime="{post.date.date().isoformat()}">{date_text(post.date, lang)}</time>'
        label = (
            f'<a href="{self.url(lang, "posts/")}">{ui["devlog"]}</a><span aria-hidden="true">·</span>{when}'
        )
        body = f"""
<article class="post">
  <header class="post-head">
    <p class="eyebrow">{label}</p>
    <h1>{escape(v.title)}</h1>
    <p class="lede">{escape(v.summary)}</p>
  </header>
  <div class="post-grid{" has-short" if aside else ""}">
    {aside}
    <div class="text">{body_html}</div>
  </div>
  {self.neighbours(post, lang)}
</article>
"""
        here = self.url(lang, f"posts/{post.slug}/")
        return self.page(
            lang, v.title, body, here=here, other=other, description=v.summary, cls="article",
            current="devlog", image=poster,
        )  # fmt: skip

    def neighbours(self, post: Post, lang: str) -> str:
        """The newer and the older post in this language, and the way back to the list."""
        ui = UI[lang]
        posts = [p for p in self.site.posts if p.get(lang)]
        i = posts.index(post)
        newer = posts[i - 1] if i else None
        older = posts[i + 1] if i + 1 < len(posts) else None
        links = []
        for p, cls in ((newer, "newer"), (older, "older")):
            if p is not None:
                v = p.get(lang)
                assert v is not None
                links.append(
                    f'<a class="{cls}" href="{self.url(lang, f"posts/{p.slug}/")}">'
                    f'<span class="eyebrow">{ui[cls]}</span><span class="to">{escape(v.title)}</span></a>'
                )
        back = f'<a class="arrow back-arrow" href="{self.url(lang, "posts/")}">{ui["back"]}</a>'
        return f'<nav class="post-nav">{"".join(links)}</nav><p class="back">{back}</p>'

    def standing(self, page: Page, lang: str) -> str:
        v = page.get(lang)
        assert v is not None
        other_lang = UI[lang]["other_lang"]
        other = self.url(other_lang, f"{page.slug}/" if page.get(other_lang) else "")
        body = f"""
<article class="post">
  <header class="post-head">
    <h1>{escape(v.title)}</h1>
    <p class="lede">{escape(v.summary)}</p>
  </header>
  <div class="post-grid"><div class="text">{md(v.body)}</div></div>
</article>
"""
        here = self.url(lang, f"{page.slug}/")
        return self.page(lang, v.title, body, here=here, other=other, description=v.summary, cls="article")

    def feed(self, lang: str) -> str:
        s, h = self.site.settings, self.site.home[lang]
        site_url = str(s.get("url", "")).rstrip("/")
        items = []
        for post in self.site.posts:
            v = post.get(lang)
            if v is None:
                continue
            link = f"{site_url}/{prefix(lang)}posts/{post.slug}/"
            html = media_links(md(v.body), f"{site_url}/", post)
            items.append(
                f"<item><title>{escape(v.title)}</title><link>{link}</link>"
                f'<guid isPermaLink="true">{link}</guid>'
                f"<pubDate>{email.utils.format_datetime(post.date)}</pubDate>"
                f"<description>{escape(v.summary)}</description>"
                f"<content:encoded>{escape(html)}</content:encoded></item>"
            )
        return (
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/" '
            'xmlns:atom="http://www.w3.org/2005/Atom"><channel>'
            f"<title>{escape(h.get('feed_title', s.get('title', '')))}</title>"
            f"<link>{site_url}/{prefix(lang)}</link>"
            f'<atom:link href="{site_url}/{prefix(lang)}feed.xml" rel="self" type="application/rss+xml"/>'
            f"<description>{escape(h.get('description', ''))}</description><language>{lang}</language>"
            f"{''.join(items)}</channel></rss>\n"
        )

    def not_found(self) -> str:
        """One page for both languages: GitHub Pages serves it for any missing address."""
        en, ru = UI["en"], UI["ru"]
        body = f"""
<section class="lost">
  <img class="lost-seal" src="{self.base}static/brand/favicon.svg" width="96" height="96" alt="">
  <p class="eyebrow">404</p>
  <h1>{en["not_found"]}</h1>
  <p class="lede" lang="ru">{ru["not_found"]}</p>
  <p class="actions">
    <a class="button" href="{self.url("en")}">{en["to_home"]}</a>
    <a class="arrow" href="{self.url("ru")}" lang="ru">{ru["to_home"]}</a>
  </p>
</section>
"""
        return self.page(
            "en", "404", body, here=f"{self.base}404.html", other=self.url("ru"), cls="lost-page"
        )
