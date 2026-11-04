# Site

The project's page on GitHub Pages (https://kaifaty.github.io/GothicAI/): what works now and the devlog,
in English (`/`) and Russian (`/ru/`), with an RSS feed for each. Built by a small generator in this folder
and published by `.github/workflows/pages.yml` when `site/` changes on `main`; pull requests only build it.

```bash
cd site
uv run gothic-site serve                       # http://127.0.0.1:47860/ (the preview "site" in .claude/launch.json)
uv run gothic-site new first-quest --title "The first quest" --title-ru "Первый квест"
uv run gothic-site frame ../.shorts/xardas-lie/take-02/main.mp4 content/posts/<post>/cover.jpg --at 41 --width 540
uv run gothic-site clip ../.shorts/xardas-wrath/take-09/short-ru.mp4 static/hero/wrath-ru.mp4 --start 33.6 --length 20
uv run gothic-site build                       # _build/ for /GothicAI/ (what the workflow publishes)
uv run pytest -q
```

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
  the YouTube uploader points to (`shorts/README.md`): change it when the uploader asks for more.

## Media

The repository keeps every file forever, so media stay small: pictures as JPEG, 540 px wide for a 9:16
frame and 1200 px for wide ones (under ~250 KB); a muted loop under 3 MB (`clip` warns). Whole shorts go
to YouTube: set `youtube` in the post, or `[hero] youtube` for the home page, and the page shows a still
that loads the video from youtube-nocookie.com only when it is clicked.

`static/brand/` is the channel's mark: the wax seal with the G as favicon (`.ico`, `.svg`), app icons and the
manifest, and `lockup.svg` (the seal and the lettering) in the bar. They are exported from the brand kit
outside the repository (`~/.local/share/gothic-ai/brand/logo/letter/kit/`): change them there and copy.

## Design

A letter written by candlelight, after the channel's brand: the night, its gold and the seal's red, with real
frames of the game as the only pictures. The method follows elayadesign's
[redesign](https://github.com/elayadesign/redesign-skill) and
[landing page](https://github.com/elayadesign/ai-design-skills) skills (one type and spacing scale, every
state, restrained motion, a sentence that lights up word by word); their own values (Geist, grey surfaces)
are a product look the brand avoids, so ours replace them. Every value is a token at the top of
`static/style.css`; a new one goes there first.

- **Colour.** Page `--night` #14110D, raised `--night-1`, lines in translucent gold. Text `--parchment`
  (headings, the npc's lines), `--ink` (running text), `--ash` (secondary). One accent, `--gold` #E2B868
  (the channel's gold): the hero's lines, actions, labels. `--wax` #9C2121 only next to the seal.
  Backgrounds stay flat; depth comes from film grain and from the take's own frame, blurred, behind it.
- **Type.** Cormorant Garamond (headings and the conversation, from 20 px up), Spectral (text), JetBrains
  Mono (labels, dates, step numbers: the machine's voice). Sizes are steps of the scale in `--t12`…`--t72`;
  weights 400 to 600, no italics outside a post's own emphasis. Headings `text-wrap: balance`, text `pretty`.
- **Space and shape.** Spacing 4, 8, 12, 16, 24, 32, 40, 48, 64, 80, 96, 128 px; sections 64 px apart on a
  phone, 96 px from 48 rem. Radius 4 (badges, code), 8 (buttons), 12 (cards, pictures), 16 (the bar, a short).
  The column is 75 rem; running text 42 rem.
- **Motion.** One curve, `--ease` (0.22, 1, 0.36, 1): 500 ms for movement, 300 ms for colour. Sections rise
  32 px and fade in once as they come into view (`data-reveal`, an `IntersectionObserver` in `site.js`);
  only `transform` and `opacity` move. With reduced motion everything only fades, the loop does not play and
  the claim shows at once.
- **Checks.** A page is looked at 390 px and 1440 px wide, in both languages: no sideways scroll, the
  primary action on the first screen, every link with hover, press and focus states.

## Publishing

Once: Settings → Pages → Source: GitHub Actions. Then a push to `main` that touches `site/` publishes it.
The page shows real takes: no edited answers, and say when pauses were cut (`shorts/README.md`).
