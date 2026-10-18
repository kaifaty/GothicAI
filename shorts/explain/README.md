# Explainer shorts

Shorts that show how a mechanic works: footage and stills from the test bench, cards with charts, labels over
what is in the picture, and captions that follow a recorded voice word by word. Rendered with
[Remotion](https://www.remotion.dev): every frame is drawn from a plan (JSON), so a number on screen is computed
from the engine's formula or taken from the bench's log, never typed into a picture.

```bash
cd shorts/explain
npm ci
npx remotion studio --props '{"brand":false}'                           # the demo plan: the dice chart, no assets
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
The renderer uses `brand/seal-corner.png` and `brand/sting.mp4` from it.

## Publication

After `npm ci` above, publish from `shorts/`:

```bash
uv run gothic-shorts explain <name-or-workspace>
uv run gothic-shorts explain "$W" --plan plan.en.json --out "$W/render/short-en.mp4"
```

The plan is relative to the workspace, as with `dub`; the workspace is a folder or a name under
`~/.local/share/gothic-ai/explain/`, as with `words` and `studio`. The default output is
`<workspace>/render/short-<plan.lang>.mp4`.

Remotion renders the muted picture (H.264, CRF 18) and three WAV stems. The command joins the picture with
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
- `brand`: boolean, defaults to `true`. A 110 × 110 G seal stays at (860, 110) over every beat, then the
  2.2-second letter sting plays after the last beat (1080 × 1920, 30 fps, its own −14 LUFS source audio).
  Publication calibrates the sting separately from the body effects through `levels`. In drafts it uses
  `brandVolume`. The voice and footage sound stop before it; the music fades out by its start. The render's duration
  includes these extra 66 frames. `false` needs no brand assets and preserves the original frames, captions
  and duration;
- `brandVolume`: draft-only nonnegative linear gain for the sting's audio, defaults to `0.355` (about −9 dB);
- `beats`: in order, each `footage` (an mp4, `freeze` and `zoom` optional), `still` (a png), `split` (two takes,
  top and bottom, with labels) or `card` (a chart); `texts` on top of a beat, `tags` (a label pinned to a point
  of the picture, to its left or right with `side`, lines appearing one by one, a roll counter that stops on the
  real number), `strokes` (a line drawing itself between two points of the picture; with `cut` it stops at a wall:
  solid up to it, dashed past it, a cross on it) and `fans` (a sector opening from a point): lines of sight and the
  bow's focus from the bench's `sight` export;
- the cards (`chart.type`): `dice` (the chance by distance, curves drawn as they are named), `formula` (the
  terms a line each as they are said, then a worked example computed the way `damagecalculator.cpp` does it:
  rounded at each step, protection not below 0, at least 5), `bars` (groups growing one by one), `dots` (marks
  filling up to a count), `stats` (the character's numbers greying out under a question); each but `stats`
  names its `source`;
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

## Look

`src/theme.ts`: the palette (the old rule grey, the new one gold, skill blue, strength red, dexterity green), the
safe area of the Shorts interface (no text in the top 150, bottom 380, right 150 px). Fonts: PT Serif Bold and
Montserrat, with full Cyrillic, shipped in `src/fonts/` under the SIL Open Font License. With `brand: true`,
all voice captions use PT Serif Bold, including words with digits. Chart labels, tags and counters keep
Montserrat; all colours stay the same. With `brand: false`, captions retain their original Montserrat.
The seal has 110 px of clearance from both the top and right edges. While it is visible, a text block,
tag or split label that touches its corner moves just below it with a 16 px gap for outlines and shadows;
other text keeps its position. Hooks, voice captions and chart cards already start below the seal.

Remotion is under the [Remotion License](https://www.remotion.pro/license): free for individuals and companies of
up to three people. The versions are pinned in `package.json`.
