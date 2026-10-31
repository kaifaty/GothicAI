# Explainer videos

## Как работает сейчас

Shorts that show how a mechanic works: footage and stills from the test bench, cards with charts, labels over
what is in the picture, and captions that follow a recorded voice word by word. Rendered with
[Remotion](https://www.remotion.dev): every frame is drawn from a plan (JSON), so a number on screen is computed
from the engine's formula or taken from the bench's log, never typed into a picture.

The plan's `format` selects `tall` (1080 × 1920, the default) or `wide` (1920 × 1080), both at 30 fps.
`Root.tsx` calculates the composition dimensions and duration from that plan. `Explainer.tsx` lays out the
picture and overlays; `parts/` draws the cards and captions. `brand: false` disables the ending and needs no
external assets. Wide diagrams live in `parts/WideDiagrams.tsx`: `rays`, `cone`, `ticks`, `grid`, and `flow`.
`parts/CombatOverlay.tsx` reads recorded actions, head tracks and HP from the public dir. Its action badge
follows the footage in either format; `parts/followMath.ts` shares the frame/zoom math with `Picture` and
maps source pixels through cover, split and zoom. These overlays are opt-in through `follow` and `hp`.
Their plan timings drive every reveal, turn, alignment and packet; the diagrams need no game or model process.
Omit a diagram beat to leave it out; these five types are rejected in tall plans. Publication of either format
uses the same `gothic_shorts.levels.publish` checks. Before them, the published footage sound fades in and out
over 30 ms at every beat's edges and at `freeze`, so a hard cut never clicks.

```bash
cd shorts/explain
npm ci
npx remotion studio --props '{"brand":false}'                           # the demo plan: the dice chart, no assets
npx remotion studio --props src/demo-wide.json                          # nineteen wide cards, no external assets
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

The game stem gets linear 30 ms fades (`EDGE_FADE`) to silence at both ends of every beat and at `freeze`,
from which Remotion's frozen video is silent. A beat may start mid-sound after a silent card, or cut
mid-sound to the next one, and still pass the click check. `gothic_shorts.explain.render` applies them to the rendered
WAV sample by sample, at the plan's beat edges with Remotion's frame rounding (`sound_spans`). They are not in
the composition: Remotion evaluates a `volume` callback once per frame, so a ramp there becomes one 33 ms step
and still clicks. The sting after the beats keeps its own start. Draft renders straight from Remotion keep
the hard cuts; `dub` gets the faded game stem through the same `render`.

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
  bow's focus from the bench's `sight` export; `paths` (a line drawing itself through several points, smoothed
  unless `curve: false`, `arrow` puts a head on its tip, `dash` makes it dashed): an arrow's flight, a route;
- motion (`src/parts/motion.ts`, one set for the series; its durations in `src/parts/timing.ts`): `texts` and
  `tags` come in with `enter`: `rise` (default, a few pixels up), `pop` (a spring with a small overshoot) or
  `slide` (from the left). Attached parts settle `MOTION.follow` (0.06 s) after their body: a tag's pin lands
  first and its box opens from it, a text's band arrives first and its words settle into it. A beat's `enter`
  (`fade`, `slide`, `wipe`, `zoom`, `flash`; `dur` defaults to 0.5 s for `slide` and `wipe`, where the whole
  frame travels, and 0.35 s for the rest) is its transition: the beat starts
  at its own time and the one before stays under it for `dur`, silent, so no time of the voice, the captions
  or the publication's edge fades moves; `flash` is a hard cut with a white flash. `zoom` eases in and out
  (cubic) unless `ease: "linear"`, for a slow push across a whole beat. The footage of a beat followed by a
  transition must run `dur` past the beat's end;
- a held picture pushes in by itself, so a long hold does not read as a frozen screen: a `card`, a `still`, or
  `footage` after its `freeze`, held 2 s or more, scales linearly across the hold by 1 % a second held, between
  3 and 5 % (`holdPush` in `timing.ts`), so a longer hold pushes slower, not further. The whole beat pushes
  about the centre of the safe area, with its tags, lines, texts and carry, so everything stays on its target;
  the captions and the hook stay still. A beat's own `zoom` replaces it (a plan cannot set both), `push: 0`
  turns it off (a beat where an exact frame matters), `push: 0.03`-`0.05` sets the scale; live footage and
  `split` do not push;
- `carry` on a beat (not the first) carries one thing across the cut into it, so the two beats share it instead
  of swapping pictures: `from: {tag, line}` takes a line of a tag of the beat before (its text, colour and the
  place `Tags` draws it on the cut, computed with `measureText`; the line leaves its box at the cut), or
  `from: [x, y]` with `text` a point of that picture. It flies along an arc (`arc`, 0.15 of the distance,
  upward; 0 is straight) to `to` (frame pixels of this beat), in-out, growing to `size`, starting `at` (0: on
  the cut) and taking `dur`, by default as long as its travel asks: 0.35 s up to 300 px, about +40 % for twice
  the distance, at most 0.6 s (`travelDur` in `timing.ts`). Onto a card its dark pill fades out on the way.
  It is drawn over the beat's own entrance, so it does not move with a `slide` or `wipe` coming in, and stays
  until `until` or the beat's end. Take `to` from a still of the card; whatever the carried thing causes (a
  point, a marker, a counter) appears on its landing, at `at` plus its duration (the strips report gives it);
- `split` keeps the same field names in either format: `src`/`from` are the first half, `srcBottom`/`fromBottom`
  the second; `labels` and `labelColors` follow that order. In `wide`, labels sit at the top of each half and
  a thin vertical line divides the pictures. Footage and stills cover their frame or half-frame;
- coordinates (`texts.y`, tag pins, strokes, fans and zoom origins) are pixels of the selected frame.
  Texts without `y` start at 260 in `tall`, 140 in `wide`; an explicit `y` is kept as supplied;
- the cards (`chart.type`): `dice` (the chance by distance, curves drawn as they are named), `formula` (the
  terms a line each as they are said, then a worked example computed the way `damagecalculator.cpp` does it:
  rounded at each step, protection not below 0, at least 5), `bars` (groups growing one by one), `dots` (marks
  filling up to a count), `stats` (the character's numbers greying out under a question), and the wide-only
  `line`, `big`, `table`, `cells`, `rays`, `cone`, `ticks`, `grid`, `flow` described below;
  each but `stats` names its `source`;
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

### `follow` and `hp` — recorded combat overlays (wide and tall)

On a footage beat, `follow` pins the current named decision above the tracked fighter's head. `hp` puts
two HP bars in the upper corner, gold for self and blue for target; 300 is full HP. Both read a workspace-relative
`actions.json` through the render's `--public-dir`, just like footage. Neither field changes existing plans.

```json
{
  "id": "duel", "kind": "footage", "src": "footage/p30-100000/duel.mp4", "from": 1, "dur": 8,
  "follow": {
    "data": "footage/p30-100000/actions.json",
    "names": {"approach": "подойти", "attack_front": "удар прямо"},
    "at": 0, "until": 8, "offsetY": -42, "hold": 1.2
  },
  "hp": {"data": "footage/p30-100000/actions.json", "labels": ["P30", "противник"], "at": 0}
}
```

`at` (default 0) and `until` gate the overlay in beat seconds. `offsetY` is in output pixels, negative
above the head (default −42). A decision starts on its source `frame`, replaces the previous decision,
and expires after `hold` source seconds (default 1.2). An action absent from `names` clears the badge.
Different names crossfade over 0.1 s. Head motion uses a deterministic centred 0.1 s triangular filter;
null tracking cuts that filter and fades the badge at its last visible position over 0.1 s.

The recording contains `fps`, `frames`, `size: [width, height]`, ordered `track: [[frame, x, y], …]`
(or `[frame, null, null]`), ordered `hp: [[frame, self_hp, target_hp], …]`, and ordered
`actions: [{frame, t, action, id}, …]`. HP uses the latest sample at or before the footage frame,
without interpolating damage. Data is validated and loaded before a frame renders; failures stop the render.

Footage time includes `from`; trimming and `freeze` use the same frame rounding as `Picture`.
Actions, tracking and HP stop with frozen footage, while the optional zoom continues as it does on the image.
Head coordinates first pass through centred `cover`, then through zoom about its local output-pixel origin.
The badge offset and text size stay in output pixels. Each split half clips its own overlays.
On a `split` beat, `follow`/`hp` use `src`/`from`; `followBottom`/`hpBottom` use `srcBottom`/`fromBottom`:
the right half in `wide`, the lower half in `tall`.

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

### `rays` — obstacle rays and observations (only wide)

A top view puts the gold fighter (`я`) at the centre. `maxCm` is the observation range in centimetres;
`rays` contains exactly 16 `{cm, hit}` entries, spaced evenly clockwise from forward (up). A hit is red
and ends at `cm`; a miss is grey and reaches `maxCm`. `raysAt: [at, dur]` reveals the rays around the
circle in order. The demo is explicitly labelled `демо`: its distances are illustrative, ready to replace
with a real `observations.jsonl` row.

`target: {x, y, label, at?}` positions the blue opponent relative to the fighter in centimetres: positive
X is right, positive Y is forward/up. `at` defaults to 0. `hideAt`, when supplied, removes the opponent
and dims the second panel with `нет данных`. `panels: [{title, lines, color, at}]` appear on the right;
use the first for self observations and the second for the opponent. `title` is optional; `source` is required.

```json
{
  "type": "rays", "title": "184 числа · демо", "maxCm": 600,
  "rays": [
    {"cm": 600, "hit": false}, {"cm": 600, "hit": false}, {"cm": 340, "hit": true}, {"cm": 280, "hit": true},
    {"cm": 250, "hit": true}, {"cm": 315, "hit": true}, {"cm": 600, "hit": false}, {"cm": 600, "hit": false},
    {"cm": 440, "hit": true}, {"cm": 600, "hit": false}, {"cm": 600, "hit": false}, {"cm": 390, "hit": true},
    {"cm": 325, "hit": true}, {"cm": 370, "hit": true}, {"cm": 600, "hit": false}, {"cm": 600, "hit": false}
  ],
  "target": {"x": 150, "y": 230, "label": "противник", "at": 0.8}, "hideAt": 5.8,
  "panels": [
    {"title": "я", "lines": ["HP · оружие · поза"], "color": "#FFD678", "at": 0.4},
    {"title": "противник", "lines": ["дальность · направление"], "color": "#7EC8FF", "at": 0.8}
  ],
  "raysAt": [1.3, 1.6],
  "source": "демо лучей; реальные данные: observations.jsonl · scripts/body_trajectory_dataset.py"
}
```

### `cone` — field of view and turn (only wide)

The fighter starts facing up. `halfDeg` sets each half of the cone in degrees; `rangeCm` sets its range.
`path: [{t, x, y}]` gives opponent positions in centimetres relative to the fighter, with the same axes as
`rays`. Times must increase strictly. Position is linearly interpolated between keys and held at the last
key afterward; the opponent appears at the first key's `t`. Outside the cone's angle or range it dims.
This is a geometric illustration, not a simulation of game collision or visibility.

Optional `turn: {at, dur, deg, label}` rotates the cone linearly over `dur` seconds; positive `deg` turns
clockwise. Its label appears beside the diagram. `texts: [{text, at, color}]` reveal short lines on the
right. `targetLabel` defaults to `противник`; `title` is optional and `source` is required.

```json
{
  "type": "cone", "title": "Конус зрения и доворот", "halfDeg": 35, "rangeCm": 420,
  "path": [{"t": 0.4, "x": 0, "y": 270}, {"t": 3, "x": 240, "y": 220}],
  "turn": {"at": 4.2, "dur": 0.4, "deg": 48, "label": "≤ 400 мс"},
  "texts": [{"text": "Доворот возвращает цель", "at": 4.6, "color": "#FFD678"}],
  "source": "docs/npc-body-combat-gate.md:109–120; reports/npc-body/2026-10-15-stage8-resume"
}
```

### `ticks` — decision and busy ticks (only wide)

Each `rows` entry has a `label`, `cells` string made only of `D` (gold decision) and `B` (grey busy body),
`at`, and optional `offset` (defaults to 0). `offset` shifts a row right by that many cell widths,
including fractions. Starting at each row's `at`, its cells appear left to right over the card's `dur`.
Letters remain only in the plan: cells show solid gold or grey, and the legend uses matching coloured
squares with the labels from `legend: {D, B}`. Optional `align: {at, dur}` linearly moves each row to
the first row's offset, keeping that reference row fixed. `title` is optional and `source` is required.

```json
{
  "type": "ticks", "title": "Один тик", "dur": 2.4,
  "rows": [
    {"label": "игра", "cells": "DBBBBDBBBBDBBBBDBBBB", "at": 0.5},
    {"label": "стенд", "cells": "DBBBBDBBBBDBBBBDBBBB", "offset": 1, "at": 0.9}
  ],
  "legend": {"D": "решение", "B": "тело занято"}, "align": {"at": 4.2, "dur": 0.8},
  "source": "reports/npc-body/2026-10-15-schedule-equivalence"
}
```

### `grid` — named actions (only wide)

`tiles: [{label, at}]` fills a grid with `columns` columns. Every tile is dark until its `at`, then lights
gold and stays lit. `title` and `source` are required. Time each tile to the corresponding spoken action;
the wide demo contains all 12 primitives, with the two pauses labelled in milliseconds.

```json
{
  "type": "grid", "title": "Действия", "columns": 2,
  "tiles": [{"label": "подойти", "at": 0.3}, {"label": "повернуться", "at": 0.7}],
  "source": "docs/npc-body-api.md:93–102"
}
```

### `flow` — loops and moving packets (only wide)

`nodes: [{id, label, x, y, w?, color?, at}]` places labelled blocks by their centre. X and Y are fractions
of the **full** 1920 × 1080 frame; `w` is also a frame-width fraction and defaults to 0.2. Keep block centres
roughly between Y=0.28 and Y=0.72 to clear the title and source. Labels accept `\n` for line breaks.
Use unique IDs; each edge must refer to existing nodes. A node defaults to gold; choose `before` grey for
past/host steps and `skill` blue for an opponent. `title` is optional and `source` is required.

`edges: [{from, to, label?, at, curve?, packets?}]` draws arrows between block boundaries over 0.6 seconds
from `at`; the arrowhead follows the drawn endpoint by arc length, including on curved arrows.
`curve` bends the arrow by a signed perpendicular fraction of the **full frame height**; zero
(the default) is straight. A positive bend from right to left goes upward, allowing a return loop over a
row of blocks. `packets: {every, dur}` starts repeated moving points after the arrow has finished drawing:
a new point starts every `every` seconds and crosses the arrow in `dur` seconds for the rest of the beat.

```json
{
  "type": "flow", "title": "Самоимитация",
  "nodes": [
    {"id": "play", "label": "Играть\n2016 дуэлей", "x": 0.2, "y": 0.56, "w": 0.22, "at": 0.4},
    {"id": "select", "label": "Отобрать\n25% лучших", "x": 0.5, "y": 0.56, "w": 0.22, "at": 1.4},
    {"id": "train", "label": "Доучить", "x": 0.8, "y": 0.56, "w": 0.22, "at": 2.4}
  ],
  "edges": [
    {"from": "play", "to": "select", "at": 1.4},
    {"from": "select", "to": "train", "at": 2.4},
    {"from": "train", "to": "play", "label": "снова", "at": 3.4, "curve": 0.23,
      "packets": {"every": 0.9, "dur": 1.5}}
  ],
  "source": "reports/npc-body/2026-10-15-self-imitation"
}
```

All new cards require `format: "wide"`. A tall plan (including the default format) fails validation
with a path such as `beats.0.chart.type` and `line: только wide`. `at` and `draw[0]` are seconds
from the beginning of their beat; durations must be positive. A small `source` line stays below each
card, leaving the bottom of the frame for captions. Plan colours should come from `src/theme.ts`: network/P30
uses `now` gold (`#FFD678`), the opponent
uses `skill` blue (`#7EC8FF`), failure uses `miss` red (`#FF5A5A`), and the past uses `before` grey
(`#D8D8D8`). Title and body fonts come from the same theme; elements appear when the voice names them.

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
The publication tests cover the default/tall/wide dimensions, all four stems and the shared levels path;
the edge-fade tests cover frame rounding, `freeze` and a WAV with hard cuts that `clicks` must find clean.
The asset-free wide demo starts with five six-second cards; representative stills are frames 150 (`dice`), 330
(`formula`), 510 (`bars`), 690 (`dots`), 870 (`stats`), and 120 (two-line captions).
It then adds seven P30 cards: `ppo-pool` (40 training points), `ppo-wall` (eight evaluations and R3m),
`throughput` (log bars), `defeats` (group colours), `network` (two big numbers and a counter), `results`
(BC/P30 table), `gate-attempts` (v1–v11, v12, v14, v16). Each names the repository data source.
Seven further beats demonstrate the diagrams: `observation-rays` (184 numbers and hidden-target state),
`vision-cone` (loss of sight and a 400 ms turn), `decision-ticks` (20 Hz), `action-grid` (12 actions),
`self-imitation-flow` (2016 duels and best 25%), `one-tick` (game/bench alignment), and `service-flow`
(Mac ⇄ Ryzen with packets). Ray distances are labelled `демо`; the cone path illustrates geometry.
Neither example supplies recorded telemetry.
Motion check: `python3 scripts/strips.py "$W/render/draft.mp4" "$W/plan.json" "$W/render/strips"` writes a strip
of consecutive frames for every planned movement (entries, transitions, cuts, lines, zooms, freezes) and a
`report.md` with the jumps nobody planned: a snap (a change in one frame while the frames around it hold) and a
one-frame flash. It also checks the motion language of the plan: a duration the plan sets off the series' set
(0.2 / 0.35 / 0.5 / 0.6 s; a line longer than 0.6 s, a linear zoom shorter than its beat, a carry outside
0.35-0.6 s), two things of one beat entering less than 0.08 s apart, a thing entering under its beat's own
transition; and lists every stretch where the picture holds still longer than `--still` (3 s by default):
nothing outside the caption band changes by 0.03 of 255 per frame (a card or a freeze changes by 0.000-0.001,
a hero idling in footage by 0.05-0.1). A slow push alone changes less than that, so a pushed hold is still
listed, with its beats and whether their push runs there (`push`: yes, part, no); a `push` below 0.03 is a
motion-language note. The draft may be a `--scale 0.5` render; only ffmpeg runs. Its tests
are `shorts/tests/test_strips.py`.
Schema and geometry checks: `node --test tests/charts.test.mjs` (Node 22.18+ for native TypeScript
stripping), including rejection of wide-only cards in tall and interpolation of `cone.path`.

Remotion is under the [Remotion License](https://www.remotion.pro/license): free for individuals and companies of
up to three people. The versions are pinned in `package.json`.

## План и журнал

- 2026-10-30: a held picture (a card, a still, a freeze) of 2 s or more pushes in by itself, 3-5 % across the
  hold (`holdPush`), the user's choice over the old rule that nothing drifts on its own; `push: 0` turns it off.
  The strips report marks which still holds it covers. On a 27 s card the title moves 1 px at a time (Chrome
  snaps text to pixels); `will-change: transform` made it smooth but cost a third of the text's sharpness.
- 2026-10-29: `strips.py` checks the motion language of the plan (durations off the set, two entrances at once,
  an entrance under a transition) and lists holds longer than `--still`, the last of the review of third-party
  motion skills (motion-art-direction, motion-video-kit's frozen-time). On the combat-AI video it found the
  cards that hold 8-11 s unchanged under the voice; the bow short's longest hold is 2.8 s.

- 2026-10-29: `carry`, a thing carried across a cut (the shared actor of motion-video-kit and onetake): a tag
  line or a point of the beat before flies along an arc into the next beat, its duration by travel.
  `parts/Carry.tsx` draws it, `parts/carryMath.ts` finds the tag line and the arc; `strips.py` gives it a strip.

- 2026-10-29: follow-through and duration by travel, from the review of third-party motion skills
  (animation-principles, motion-video-kit, Disney rules): a tag's box follows its pin and a text's words follow
  their band by `MOTION.follow` (0.06 s); `slide` and `wipe` transitions default to 0.5 s instead of 0.35 s,
  since the whole frame travels (an explicit `dur` in a plan is kept). The durations moved to
  `src/parts/timing.ts`, shared by the schema; `scripts/strips.py` uses the same defaults. A plan that relied on
  the old 0.35 s slide needs its footage to run 0.5 s past the beat or an explicit `dur: 0.35`.

- 2026-10-25: added the motion set: `enter` for texts and tags (`rise`, `pop`, `slide`), beat transitions
  (`fade`, `slide`, `wipe`, `zoom`, `flash`) that keep every time of the plan, `paths` drawn through points with an
  arrowhead (`@remotion/paths`), an eased `zoom`, and `scripts/strips.py`. Tried on the combat-AI short.

- 2026-10-19: the published game stem fades over 30 ms at every beat edge and at `freeze`, sample by sample
  in `explain.render`. The combat-AI video's click at 432.27 s needed per-beat `afade` copies of each clip;
  plans no longer need them. A ramp in Remotion's per-frame `volume` only moved the click by one frame.
- 2026-10-19: added opt-in `follow`/`hp` and split `followBottom`/`hpBottom`, with recorded action
  names, hold/crossfade, smoothed head tracking, HP samples and shared cover/zoom/freeze math.
- 2026-10-19: removed tick-cell letters and replaced legend letters with coloured squares;
  curved flow arrowheads now follow the drawn arc length, with a 2 px regression tolerance.
- 2026-10-19: added wide-only `rays`, `cone`, `ticks`, `grid`, `flow` with voice-relative reveals,
  target hiding, field-of-view turns, tick alignment, curved loops and recurring packets. The wide demo
  adds seven storyboard examples for P30; existing tall cards retain their original layout.

- 2026-10-19: added wide-only `line`, `big`, `table`, `cells`, logarithmic bars and per-group colours/notes;
  the wide demo includes the P30 learning curves, throughput, defeats, model size, final evaluation and gate.
- 2026-10-19: added plan-driven `tall`/`wide` dimensions, landscape media, split, captions and cards,
  centred vertical branding, wide publication and the asset-free five-card demo. Tall remains the default.
