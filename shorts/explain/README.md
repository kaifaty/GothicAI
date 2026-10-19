# Explainer videos

## Как работает сейчас

Shorts that show how a mechanic works: footage and stills from the test bench, cards with charts, labels over
what is in the picture, and captions that follow a recorded voice word by word. Rendered with
[Remotion](https://www.remotion.dev): every frame is drawn from a plan (JSON), so a number on screen is computed
from the engine's formula or taken from the bench's log, never typed into a picture.

The plan's `format` selects `tall` (1080 × 1920, the default) or `wide` (1920 × 1080), both at 30 fps.
`Root.tsx` calculates the composition dimensions and duration from that plan. `Explainer.tsx` lays out the
picture and overlays; `parts/` draws the cards and captions. `brand: false` disables the ending and needs no
external assets. Publication of either format uses the same `gothic_shorts.levels.publish` checks.

```bash
cd shorts/explain
npm ci
npx remotion studio --props '{"brand":false}'                           # the demo plan: the dice chart, no assets
npx remotion studio --props src/demo-wide.json                          # twelve wide cards, no external assets
npx remotion still Explainer --props src/demo-wide.json --frame 330 out/wide-formula.png
npx remotion still Explainer --public-dir "$W" --props "$W/plan.json" --frame 120 "$W/render/f120.png"
npx remotion render Explainer --public-dir "$W" --props "$W/plan.json" --codec h264 --crf 18 "$W/render/preview.mp4"
```

`$W` is the short's own folder (outside the repository): footage, stills and voice lines referenced by the plan
live there and are served as the public dir.

Connect the channel assets to that folder once, before rendering with branding enabled:

```bash
ln -s "$HOME/.local/share/gothic-ai/brand/sting" "$W/brand"
```

Remotion serves files through this directory symlink with `--public-dir "$W"`; no copying is needed.
The renderer uses `brand/sting.mp4` from it.

## Publication

After `npm ci` above, publish from `shorts/`:

```bash
uv run gothic-shorts explain <name-or-workspace>
uv run gothic-shorts explain "$W" --plan plan.en.json --out "$W/render/short-en.mp4"
```

The plan is relative to the workspace, as with `dub`; the workspace is a folder or a name under
`~/.local/share/gothic-ai/explain/`, as with `words` and `studio`. The default output is
`<workspace>/render/short-<plan.lang>.mp4`.

Remotion renders the muted picture at the plan's dimensions (H.264, CRF 18) and three WAV stems. The command joins the picture with
the game/ending stem, takes each speech window from the first to the last word of a nonempty `lines` entry,
and calls `gothic_shorts.levels.publish`. That shared function calibrates the voice, effects, music and ending,
masters the mix, and checks the decoded AAC: voice −14 LUFS, true peak ≤ −1 dBTP, the balance between stems,
and zero clicks. It prints the table and writes a sibling `.levels.json` report. A failed check returns a
nonzero exit code and removes the output video. Temporary pictures and stems are removed on success or failure.
Without `music` in the plan, music rows have no data, as in the shared publisher.

`stem` is optional: `voice` plays only the voice lines at 1; `game` plays the footage at each beat's `game`
level, then the brand sting at 1; `music` plays the looped bed at 1 with its `fadeIn`/`fadeOut`. Stems have no
ducking or plan mix gains. All stems cover the full video, including the sting; music and voice stop at the
end of the beats. Music fades are already rendered, so publication passes `music_fades=False` to `levels`.
The body duration is the summed beats rounded to frames; the full length adds `STING_FRAMES` (66) when branded.

For publication, `gameVolume`, `duck`, `music.volume`, `music.duck` and `brandVolume` are ignored: `levels`
sets the levels. `beat.game` remains a relative level within the effects stem. Record footage with `MUSIC=0`
and sound effects enabled, then add one music bed in the plan. Direct Remotion renders without `stem` keep
the original plan gains and ducking for Studio and drafts; they do not run the publication checks.

## The plan

`src/schema.ts` is the format; a missing or misspelt field stops the render with its path. In short:

- `hook`: the big text over the first seconds;
- `format`: `tall` (default, 1080 × 1920) or `wide` (1920 × 1080). The same `Explainer` composition and
  `gothic-shorts explain` command handle both; there is no short-video duration limit;
- `brand`: boolean, defaults to `true`. The 2.2-second letter sting plays after the last beat
  (1080 × 1920, 30 fps, its own −14 LUFS source audio). In `wide`, this vertical video is fitted to the
  full frame height, centred on black without cropping or stretching;
  Publication calibrates the sting separately from the body effects through `levels`. In drafts it uses
  `brandVolume`. The voice and footage sound stop before it; the music fades out by its start. The render's duration
  includes these extra 66 frames. `false` needs no brand assets and preserves the original frames, captions
  and duration;
- `brandVolume`: draft-only nonnegative linear gain for the sting's audio, defaults to `0.355` (about −9 dB);
- `beats`: in order, each `footage` (an mp4, `freeze` and `zoom` optional), `still` (a png), `split` (two takes,
  top/bottom in `tall`, left/right in `wide`, with labels) or `card` (a chart); `texts` on top of a beat, `tags` (a label pinned to a point
  of the picture, to its left or right with `side`, lines appearing one by one, a roll counter that stops on the
  real number), `strokes` (a line drawing itself between two points of the picture; with `cut` it stops at a wall:
  solid up to it, dashed past it, a cross on it) and `fans` (a sector opening from a point): lines of sight and the
  bow's focus from the bench's `sight` export;
- `split` keeps the same field names in either format: `src`/`from` are the first half, `srcBottom`/`fromBottom`
  the second; `labels` and `labelColors` follow that order. In `wide`, labels sit at the top of each half and
  a thin vertical line divides the pictures. Footage and stills cover their frame or half-frame;
- coordinates (`texts.y`, tag pins, strokes, fans and zoom origins) are pixels of the selected frame.
  Texts without `y` start at 260 in `tall`, 140 in `wide`; an explicit `y` is kept as supplied;
- the cards (`chart.type`): `dice` (the chance by distance, curves drawn as they are named), `formula` (the
  terms a line each as they are said, then a worked example computed the way `damagecalculator.cpp` does it:
  rounded at each step, protection not below 0, at least 5), `bars` (groups growing one by one), `dots` (marks
  filling up to a count), `stats` (the character's numbers greying out under a question), and the wide-only
  `line`, `big`, `table`, `cells` described below; each but `stats` names its `source`;
- `lines`: the voice's words with their times, a sentence per line (`gothic-shorts words`); the captions are
  pages of 2-4 words of them, the word being said in gold. A page never ends on a preposition or a conjunction
  (`в`, `как`, `of the`, `than`): it goes to the next one. For captions in another language than the voice, the
  plan's maker spreads the translated words over the spoken ones (a part per pause);
- `voice`: the recorded lines and where they start; in drafts the game's sound ducks under them (`duck`),
  and `gameVolume` sets it as a whole (0: the draft footage is silent). Publication uses the word windows
  and shared `levels` rules instead;
- `music`: one looped bed under the beats (`file`, `fadeIn`, `fadeOut`); `volume` in the voice's pauses and
  `duck` times that while it speaks apply only to drafts. Publication calibrates and ducks this stem in `levels`.

`{chance:exe:40:16}` in a tag is the chance by that rule (`exe`: Gothic2.exe's dice, `opengothic`: OpenGothic's
before it) for that talent and distance, computed by `src/dice.ts` from the same formulas as
`OpenGothic/common/game/damagecalculator.cpp`.

### `line` — point-driven lines (only wide)

Each series has a `name`, `color`, at least two `points: [[x, y], …]` with strictly increasing X,
and `draw: [at, dur]`. The line and its points reveal linearly from the first to the last X in `dur`
seconds; the legend appears at `at`. Series have independent timings. `axisX` and `axisY` each specify
`min`, `max` (greater than `min`), `ticks` (numeric positions) and `label` (including the unit).
Points and ticks must be inside their axis ranges. Values use a decimal comma and tabular figures.

```json
{
  "type": "line", "title": "Победы над пулом, %",
  "axisX": {"min": 0, "max": 40, "ticks": [0, 10, 20, 30, 40], "label": "итерация"},
  "axisY": {"min": 0, "max": 100, "ticks": [0, 50, 100], "label": "победы, %"},
  "series": [{"name": "Пул", "color": "#FFD678", "points": [[1, 50.9], [40, 90.4]], "draw": [0.5, 4]}],
  "marks": [{"x": 30, "text": "P30", "at": 4.8}],
  "labels": [{"x": 40, "y": 90.4, "text": "90,4%", "at": 4.6}],
  "source": "reports/npc-body/2026-10-16-ppo/iterations.json · train_pool"
}
```

`marks` draw a vertical dashed line at X; `labels` put text beside a data coordinate. Both arrays
default to empty, and each entry appears at its own `at`. Edge labels stay inside the plot; keep
annotations short and separated. `demo-wide.json` contains the full 40-point curve (ratios converted
to percent without rounding), plus wall HP every five iterations with the R3m starting value at X=0.

### `bars` — compatible extensions

Existing plans and linear heights keep their layout. `log: true` uses logarithmic height, useful for
the 2.5 → 786 decisions/s demo. The baseline is the smaller of 1 and the power of ten immediately below
the smallest positive value: height is `log(value / baseline) / log(max / baseline)`. The baseline and
logarithmic scale are printed on the card. Zero has no bar and stays on that baseline, with an explicit
note on the scale. With `log`, `max` must be positive and values must be finite and between 0 and `max`.

Groups may add `color` (overrides the series colour for bars and their values) and `note` (small print
below the group label, appearing at the same `at`). Wide bars with notes reserve space for two-line
labels and notes above `source`. These extensions also work in tall.

```json
{
  "type": "bars", "title": "Решений в секунду", "log": true,
  "series": [{"name": "Скорость", "color": "#D8D8D8"}],
  "groups": [
    {"label": "16 runner", "values": [90], "at": 0.5, "note": "без модели"},
    {"label": "Fork prefix", "values": [786], "at": 1.5, "color": "#FFD678"}
  ],
  "max": 1000, "format": "{v}", "source": "docs/npc-body-control-plan.md §9.1"
}
```

### `big` — one to three large numbers (only wide)

`items` contains one to three entries in a row: `value` (string or number), `label`, `at`, optional
`color`. A numeric value uses decimal commas and grouped thousands; strings keep their spelling.
An optional `title` heads the card; `source` sits below it.

An item's optional `count: {from, to, dur, format}` replaces its static `value` with a linear counter
starting at the item's `at`, reaching and keeping `to` after `dur` seconds. In `format`, `{v}` is a
grouped integer, `{v:1}` … `{v:6}` fixes that many decimal places; surrounding text is kept.

```json
{
  "type": "big", "title": "Маленькая сеть",
  "items": [
    {"value": 476556, "label": "чисел в сети", "at": 0.3, "color": "#FFD678",
     "count": {"from": 0, "to": 476556, "dur": 2.5, "format": "{v}"}},
    {"value": "≈1 мс", "label": "на решение", "at": 3}
  ],
  "source": "body-service/src/gothic_body/models.py; reports/npc-body/2026-10-16-gate-r3m-p30"
}
```

### `table` — timed rows (only wide)

`columns` are header strings. Every `rows` entry has `cells` (strings or numbers, exactly as many as
columns) and `at`; the whole row appears together. `highlight` is an optional **zero-based** column
index, colouring its header and cells gold. Numeric cells use tabular figures and Russian number
formatting; use strings for a fixed number of decimal places or units. `title` is optional, `source`
is required.

```json
{
  "type": "table", "title": "BC против P30", "columns": ["Метрика", "BC", "P30"], "highlight": 2,
  "rows": [
    {"cells": ["HP из 300", "218,2", "294,5"], "at": 0.5},
    {"cells": ["Без урона, из 2048", 78, 1814], "at": 1.6},
    {"cells": ["Средняя победа, с", "15,3", "9,7"], "at": 2.7}
  ],
  "source": "reports/npc-body/2026-10-16-ppo/README.md · «Итоговая оценка P30»"
}
```

### `cells` — exam attempts (only wide)

Each `cells` entry has `label`, `state: "fail" | "pass" | "skip"`, and `at`. Cells appear in a single
strip: red ×, green ✓, or grey —, so state is readable without colour. `spans` label inclusive,
**zero-based** ranges `{from, to, text}` above the strip and appear when the first cell in that range
appears. `notes: [{text, at, color?}]` appear below, one line each. Both arrays default to empty.
`title` is optional, `source` is required. Keep labels and notes short enough for the strip.

```json
{
  "type": "cells", "title": "Экзамен",
  "cells": [{"label": "v14", "state": "fail", "at": 0.5}, {"label": "v16", "state": "pass", "at": 2}],
  "spans": [{"from": 0, "to": 1, "text": "15.10"}],
  "notes": [{"text": "Пороги не ослабляли", "at": 2.5, "color": "#8BE08B"}],
  "source": "docs/npc-body-control-plan.md · этап 8"
}
```

All new cards require `format: "wide"`. A tall plan (including the default format) fails validation
with a path such as `beats.0.chart.type` and `line: только wide`. `at` and `draw[0]` are seconds
from the beginning of their beat; durations must be positive. A small `source` line stays below each
card, leaving the bottom of the frame for captions. Plan colours should come from `src/theme.ts`.

## Look

`src/theme.ts`: the palette (the old rule grey, the new one gold, skill blue, strength red, dexterity green), the
safe area of the Shorts interface (no text in the top 150, bottom 380, right 150 px). Fonts: PT Serif Bold and
Montserrat, with full Cyrillic, shipped in `src/fonts/` under the SIL Open Font License. With `brand: true`,
all voice captions use PT Serif Bold, including words with digits. Chart labels, tags and counters keep
Montserrat; all colours stay the same. With `brand: false`, captions retain their original Montserrat.

`wide` uses 5% margins instead of the Shorts safe area. Captions sit at the bottom at up to 56 px and fit
within two lines; unusually long pages reduce the font size. Hooks use 80 px near the top, texts use the
full inner width, and cards have landscape geometry: a wide dice plot, formula terms beside the worked
example, grouped bars, a ten-column dot grid and a centred stats panel. The vertical brand sting remains
pillarboxed until a horizontal asset is available.

Validation: `npm run typecheck` in this folder and `uv run pytest -q tests/test_explain.py` in `shorts/`.
The publication tests cover the default/tall/wide dimensions, all four stems and the shared levels path.
The asset-free wide demo starts with five six-second cards; representative stills are frames 150 (`dice`), 330
(`formula`), 510 (`bars`), 690 (`dots`), 870 (`stats`), and 120 (two-line captions).
It then adds seven P30 cards: `ppo-pool` (40 training points), `ppo-wall` (eight evaluations and R3m),
`throughput` (log bars), `defeats` (group colours), `network` (two big numbers and a counter), `results`
(BC/P30 table), `gate-attempts` (v1–v11, v12, v14, v16). Each names the repository data source.
Schema checks: `node --test tests/charts.test.mjs` (Node 22.18+ for native TypeScript stripping).

Remotion is under the [Remotion License](https://www.remotion.pro/license): free for individuals and companies of
up to three people. The versions are pinned in `package.json`.

## План и журнал

- 2026-10-19: added wide-only `line`, `big`, `table`, `cells`, logarithmic bars and per-group colours/notes;
  the wide demo includes the P30 learning curves, throughput, defeats, model size, final evaluation and gate.
- 2026-10-19: added plan-driven `tall`/`wide` dimensions, landscape media, split, captions and cards,
  centred vertical branding, wide publication and the asset-free five-card demo. Tall remains the default.
