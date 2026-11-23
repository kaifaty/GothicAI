# Site

The project's page on GitHub Pages (https://kaifaty.github.io/GothicAI/): what works now and the devlog,
in English (`/`) and Russian (`/ru/`), with an RSS feed for each. Nuxt 4 generates it as static pages (SSG)
and `.github/workflows/pages.yml` publishes it when `site/` changes on `main`; pull requests only build it.

```bash
cd site
bun install
bun run dev                                    # http://localhost:47860/ (the preview "site" in .claude/launch.json)
bun run new first-quest --title "The first quest" --title-ru "Первый квест"
bun run frame /path/to/takes/xardas-lie/take-02/main.mp4 content/posts/<post>/cover.jpg --at 41 --width 540
bun run clip /path/to/takes/xardas-wrath/take-09/short-ru.mp4 static/hero/wrath-ru.mp4 --start 33.6 --length 20
bun run generate                               # .output/public/ for /GothicAI/ (what the workflow publishes)
bun run preview                                # serves .output/public under its base path, compressed, at :47861
bun run test                                   # generates the site, then checks the files (vitest)
```

`bun run dev` serves the site at `/`; `generate` builds it for `base` in `home.toml` (`/GothicAI/`), and
`NUXT_APP_BASE_URL=/ bun run generate` builds it for `/`. `frame` and `clip` need ffmpeg.

## How it is built

- **No client Vue.** `features.noScripts` sends every page as plain HTML and CSS: no Nuxt scripts, no
  payload. The one script is ours, `app/assets/site.js` (the short, the reveals, the claim, the backgrounds
  below the first screen), minified and inlined at the end of each page, plus a line in `<head>` that
  shows everything if it does not run within 2.5 s. `server/plugins/page.ts` drops Nuxt's speculation
  rules, so a page holds no `<script>` but these two (`tests/site.test.ts` checks it).
- **Content at build time.** `lib/content.ts` reads `content/` (smol-toml, markdown-it with typographic
  quotes and tables) with the same checks as before: required fields, a date with a time zone, the slug
  from the folder's name, no repeated slug, the cover exists. `modules/site.ts` hands it to the pages as
  `#site-content`, publishes `static/` at `/static/` and each post's pictures at `/media/<slug>/`, lists
  every page to prerender and serves the feeds (`server/feeds/`). A change under `content/` restarts `dev`.
- **Pages.** `app/pages/` are thin: `HomeView`, `DevlogView`, `PostView`, `StandingView` and `error.vue`
  (the one `404.html`) in `app/components/`, around `SitePage` (head, `SiteBar`, `SiteFoot`), with
  `ShortPlayer`, `PostCards`, `DialogueScene`, `ColonyPeople`, `DiaryFlow` and `SitePicture`. They print the
  same classes and markup the CSS was written for.

## Content

- `content/home.toml`: the settings (`[site]`: base path, address, repository, Telegram), the short on the
  home page (`[hero]`), and the home page's text per language (`[en]`, `[ru]`): the conversation shown
  first, the pitch, what works now, how a conversation goes.
- `content/posts/<date>-<slug>/en.md` and `ru.md` (Russian optional, English required), with TOML front
  matter between `+++` lines: `title`, `summary` (one sentence: the list and the feed), `date` with a time
  zone, optionally `cover` (a picture in the post's folder) and `youtube` (a video id). Pictures next to
  the post are linked by their name: `![Frames of the short](sheet-en.jpg)`.
- `content/pages/<slug>/en.md` and `ru.md`: pages outside the devlog at `/<slug>/`, linked from every
  footer, with `title` and `summary` only. `privacy` is the privacy policy that the Google Cloud project of
  the YouTube uploader points to (the private `kaifaty/gothic-shorts` repository): change it when the uploader asks for more.

## Media

The repository keeps every file forever, so media stay small: pictures as JPEG, 540 px wide for a 9:16
frame and 1200 px for wide ones (under ~250 KB); a muted loop under 3 MB (`clip` warns). The build makes
the smaller copies the pages show (below); the files themselves are published as they are. Whole shorts go
to YouTube: set `youtube` in the post, or `[hero] youtube` for the home page, and the page shows a still
that loads the video from youtube-nocookie.com only when it is clicked.

`static/brand/` is the channel's mark: the wax seal with the G as favicon (`.ico`, `.svg`), app icons and the
manifest, and `lockup.svg` (the seal and the lettering) in the bar. They are exported from the brand kit
outside the repository (`~/.local/share/gothic-ai/brand/logo/letter/kit/`): change them there and copy.

## Design

A letter written by candlelight in the colony, after the channel's brand: the night, its gold and the seal's
red; the valley under the Barrier, its people and the hero's diary as illustrations; real frames of the game as
the proof. The method follows elayadesign's
[redesign](https://github.com/elayadesign/redesign-skill) and
[landing page](https://github.com/elayadesign/ai-design-skills) skills (one type and spacing scale, every
state, restrained motion, a sentence that lights up word by word); their own values (Geist, grey surfaces)
are a product look the brand avoids, so ours replace them. Every value is a token at the top of
`app/assets/style.css`; a new one goes there first. The CSS is inlined into every page (about 26 KB, and
12 KB of `@font-face`): no request stands between the HTML and the first paint.

- **Colour.** Page `--night` #14110D, raised `--night-1`, lines in translucent gold. Text `--parchment`
  (headings, the npc's lines), `--ink` (running text), `--ash` (secondary). One accent, `--gold` #E2B868
  (the channel's gold): the hero's lines, actions, labels. `--wax` #9C2121 only next to the seal.
  Backgrounds stay flat; depth comes from film grain, the illustrations and the take's own frame, blurred,
  behind it.
- **Illustrations** (`static/art/`) are ours, drawn with `image_gen` for the site: the valley under the
  Barrier (`valley` on the first screen, `valley-2` at the top of inner pages and under the footer), the old
  mage beside the claim, four people of the colony (`miner`, `guard`, `novice`, `hunter`, named in
  `home.toml` `people`), the parchment of the diary page. Prompts, full-size sources and the choice are in
  `reports/site/2026-11-gothic-look/`. They are never passed off as the game: the people row and the footer
  say they are illustrations, and every frame of the game is a real take. WebP, about 30 to 110 KB each; their
  edges melt into the night with masks, and the diary page's torn edges are masks too (`art/torn-*.svg`).
- **Type.** Cormorant Garamond (headings and the conversation, from 20 px up), Spectral (text), JetBrains
  Mono (labels, dates, step numbers: the machine's voice). Sizes are steps of the scale in `--t12`…`--t72`;
  weights 400 to 600, no italics outside a post's own emphasis. Headings `text-wrap: balance`, text `pretty`.
  The fonts are the site's own: `@nuxt/fonts` downloads Cormorant Garamond 500/600, Spectral 400/500/600
  and 400 italic, JetBrains Mono 400/500 from Google Fonts when the site is built (latin, latin-ext and
  cyrillic, woff2, `font-display: swap`) and serves them from `/_nuxt/fonts/`; the reader's browser asks no
  font host. A face loads only when its letters are on the page (`unicode-range`). The headline's and the
  running text's faces in the page's language are preloaded (`server/plugins/page.ts`); while a face loads,
  a local one with matched metrics stands in, so the text does not jump.
- **Pictures.** `@nuxt/image` makes AVIF copies with a WebP fallback while the pages are prerendered
  (`app/components/SitePicture.vue`), each with `width` and `height`: the first screen's valley no wider
  than 800 px on a phone, preloaded with high priority; portraits and the cards' covers (~280 px, 1x/2x) in
  the widths the layout needs, loading lazily. The video's still is one WebP (540 px), the glow behind it a
  120 px copy (it is blurred by 56 px). The valley behind the inner pages and the footer is `--valley-2`,
  640 px on a phone and the whole picture from 48 rem. The short's loop asks for nothing until it is near
  the screen (`preload="none"`, its address set by `site.js`); the diary's paper and the footer's valley
  are asked for one screen before they show. Pictures in a post's Markdown load lazily.
- **Space and shape.** Spacing 4, 8, 12, 16, 24, 32, 40, 48, 64, 80, 96, 128 px; sections 64 px apart on a
  phone, 96 px from 48 rem. Radius 4 (badges, code), 8 (buttons), 12 (cards, pictures), 16 (the bar, a short).
  The column is 75 rem; running text 42 rem.
- **Motion.** One curve, `--ease` (0.22, 1, 0.36, 1): 500 ms for movement, 300 ms for colour. Sections rise
  32 px and fade in once as they come into view (`data-reveal`, an `IntersectionObserver` in
  `app/assets/site.js`);
  only `transform` and `opacity` move. With reduced motion everything only fades, the loop does not play and
  the claim shows at once.
- **Checks.** A page is looked at 390 px and 1440 px wide, in both languages: no sideways scroll, the
  primary action on the first screen, every link with hover, press and focus states. Its weight is measured
  on a cold load (`~/.codex/checks/gothic/site-weight.py` against `bun run preview`): a phone on slow 4G gets
  the first screen's LCP within 2 s and under 250 KB before scrolling; a desktop under 500 KB, the loop not
  loaded; nothing from another host.

## Publishing

Once: Settings → Pages → Source: GitHub Actions. Then a push to `main` that touches `site/` publishes it.
The page shows real takes: no edited answers, and say when pauses were cut (the private `kaifaty/gothic-shorts` repository).
