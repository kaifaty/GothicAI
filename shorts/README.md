# Shorts

Vertical videos of the game with the real AI npc's, made without a person at the keyboard: a shot file is a
scene with the hero's lines, the hero says them with a TTS voice through the talk key, the npc's hear them
through speech recognition and answer with the language model, the test bench films it with the game's
effects and separate AI npc voices, and the cut makes a 1080x1920 short with subtitles.

The [explainer renderer](explain/README.md) also makes long 1920×1080 videos: set `format: "wide"` in its
JSON plan and publish with `gothic-shorts explain`. Omitted `format` keeps the vertical 1080×1920 layout.

Game music is not recorded: `film` always writes `musicVolume=0` in the take's `Gothic.ini`, including
for old shots with a nonzero `music` value, and `worldSounds=0`: the map's looping place sounds
(zCVobSound: wind and birds, surf, torches) would be a second tune under the bed. The cut adds one music bed for the whole short, starting
at its `clean_from_s` in the local `catalog.tsv`; set `bed_start` in the shot when no clean start is
catalogued. All three Xardas shots use `xardas-tower`.

The hero and each NPC are levelled by speaker using all their recorded lines. Their combined voice stem
is cut on the same timeline as the picture and effects. `levels.publish` calibrates the effects, music
and ending against that voice, masters it to −14 LUFS, and checks the decoded AAC (true peak and clicks
included). All stems are averaged to dual mono before levelling and loudness measurements, keeping a
stereo layout. Published AAC has two identical channels; the `dual_mono` gate requires max L−R ≤ −90 dBFS.
AAC uses AudioToolbox where available; the FFmpeg fallback uses 256k with PNS and intensity stereo
disabled and M/S forced to keep noisy audio in dual mono.
It prints the levels table and writes `short-<lang>.levels.json`; a failed check leaves no
published MP4. Old takes without NPC stems or verified `musicVolume=0` and `worldSounds=0` must be filmed again. `--lang en`
changes the captions and keeps the same sound.

Channel branding is on by default: PT Serif Bold text and the end sting (see below).
Set `brand = false` in the shot TOML to turn it off.

```bash
cd shorts
uv run gothic-shorts film xardas-lie            # a take: .shorts/xardas-lie/take-NN/
uv run gothic-shorts film xardas-lie --takes 3  # three takes in a row (the answers differ every time)
uv run gothic-shorts cut xardas-lie             # the newest take -> short-ru.mp4 (+ .png sheet, .md transcript)
uv run gothic-shorts cut xardas-lie 2           # take-02
uv run gothic-shorts subs xardas-lie --lang en  # subs.en.json of the take: fill in the translations, then
uv run gothic-shorts srt xardas-lie             # -> subs.en.srt: English captions for the Russian short
uv run gothic-shorts dub xardas-lie             # -> dub.en.wav: its English sound track (+ dub.en.mp4 to check)
uv run gothic-shorts cut xardas-lie --lang en   # -> short-en.mp4 (English burnt in; no longer uploaded)
```

What it uses: the launcher's settings and keys (`gothic-launcher config`: ChatGPT sign-in, the ASR and TTS
providers), the engine build of this checkout (`scripts/build-engine.sh`), the game folder, and the bench
save (`save_slot_1.sav` of the main checkout, Xardas' tower) unless the shot names another. The services run
on free ports, so a play session can run at the same time; the game window is minimized and silent.

## A shot

`shots/<name>.toml`:

```toml
title = "Соврал Ксардасу"
hook = "Я попробовал обмануть Ксардаса"   # big text over the first seconds
npc = "NONE_100_XARDAS"                   # {npc} in the script
music = 0.0                               # the default; film keeps game music off
bed = "khorinis-countryside"              # ~/.local/share/gothic-ai/music/game/<bed>.flac
# bed_start = 12.0                        # otherwise catalog.tsv's clean_from_s
sound = 1.0                               # the game's effects volume, 0..1
# save = "my.sav"                         # relative to the shot file
# hero_voice = "A young man ..."          # a description: the voice is designed, not cloned
script = """
overlay off
ai attach {npc}          # the npc stops and stands: it answers through the game's own queue
wait 500
near {npc} 170           # the hero 1.7 m in front of it, facing it
cam shoulder {npc} 130 15 1
rec main                 # the recorded segment must be called main
hero Ксардас, меня прислал Ли.   # the hero says it
waitreply 40000          # until the npc has answered and is quiet
cam face {npc}
hero Ну, хоть пару зелий.
waitreply 40000
stop
"""
```

The script is the bench's language (`scripts/bench/README.md`) plus `hero <text>`. The commands made for
shots:

| Command | |
|---|---|
| `hero <text>` | the hero says it: a TTS recording held on the talk key (the bench's `ptt <wav> <text>`) |
| `waitreply [timeout ms] [quiet ms]` | until an npc of the AI has answered the hero's last phrase and has been quiet for `quiet` (1200) |
| `waitquiet [quiet ms] [timeout ms]` | until no npc of the AI speaks |
| `near <npc> [dist cm] [deg]` | the hero stands in front of the npc (off its facing by `deg`), facing it |
| `approach <npc>` | the hero walks up to the npc; then `waitarrive` |
| `face npc <npc>` | the hero turns to the npc |
| `cam shoulder <npc> [dist] [height] [1\|-1]` | over the hero's shoulder at the npc's face |
| `cam face <npc> [dist] [height] [deg]` | the npc's face from the front, turned towards the hero |
| `cam two <npc> [dist] [height] [1\|-1]` | both from the side |
| `cam cut`, `cam smooth <0..1>` | the next shot jumps instead of gliding; how fast the camera glides |
| `caption <text>`, `caption off` | text at the top of the short from here |
| `ai attach <npc>`, `ai detach <npc>` | the bridge's console: the npc is held (stands, answers in the game's queue) |
| `ai skip <npc>` | its important dialogs count as told: it does not stop the hero with the game's own greeting |

## Making a short (the agent's loop)

1. Write the shot: what the short shows in one sentence (the hook), the hero's lines, the camera. Keep it
   under 60 s; one idea per short.
2. `film --takes 2..3`. The summary of each take lists what the hero said, what speech recognition heard,
   and every npc line; a take with `timed out` lost an answer (the model was overloaded, nobody heard).
3. Pick the take whose answers carry the idea, `cut` it, read the transcript (`short-ru.md`) and look at
   the sheet (`short-ru.png`); look at single frames when something is off
   (`ffmpeg -ss 12 -i short-ru.mp4 -frames:v 1 f.png`).
4. If the answers miss: change the hero's lines (the npc answers what it heard), not the cut.
5. English: `subs`, translate every text in `subs.en.json`, `srt` -> `subs.en.srt`, the caption track the
   upload adds to the Russian short; `dub` -> `dub.en.wav`, the English sound track (a line it reports as
   too long is shortened in `subs.en.json`, then `srt` and `dub` again). Watch `dub.en.mp4` before the
   upload. A short is one video in every language (see Uploading to YouTube).

Honesty: the npc's answers are not edited, only the pauses while the model thinks are cut down (to ~0.5 s).
Say so in the description.

## How it works

- `film` voices the hero's lines once (`tts-service`, `gothic-tts say`, the voice `SHORTS_HERO` designed from
  `hero_voice`; kept in `.shorts/voice/` by text), writes the bench scenario, starts ASR, TTS and the AI
  server like the launcher (with `--voice-files`: a `ptt_start` may name a wav instead of the microphone),
  and the game with the bench in film mode. TTS always starts with `--no-original-voices`: shorts keep
  designed NPC voices regardless of the launcher's `tts.original_voices` setting:
  - `TEMPEST_WINDOW_SIZE`: the frame (486x864 points, 972x1728 pixels);
  - `TEMPEST_AUDIO_LOOPBACK=48000`: nothing plays; OpenAL mixes the game's effects when the bench asks, a
    frame's worth per frame, into `main.wav` next to `main.mp4`;
  - `OPENGOTHIC_BENCH_VOICE_STEM=1`: AI npc voices stay out of `main.wav`; each line starting during a
    segment gets its own `<segment>.voice-NN.wav` (16-bit mono, at the voice buffer's sample rate), cut
    off at the segment's end. `npc_voice` in `events.jsonl` gives the npc, line id, absolute wav path and
    starting sample in the 48 kHz segment, alongside the usual video frame;
  - `OPENGOTHIC_BENCH_REALTIME`: the fixed steps keep to the wall clock (the AI server lives in real time);
    the window is not drawn, the captured frame is the frame; `rec stop ... speed=99%` in `bench.log` says
    how well the game kept up (below ~95% lines said on the move start too early);
  - `OPENGOTHIC_BENCH_ENCODER`: the hardware encoder (x264 slows the game down).
- The bench writes `events.jsonl`: the hero's phrases, what the server heard, every npc line as it starts
  and its voice's end, captions, in frames of the recording.
- `cut` builds the timeline (`timeline.py`), cuts the wait between the hero's phrase and the first answer,
  cuts the hero/NPC voice stem and effects with matching soft joins, and burns in the hook, captions and
  subtitles as images (Pillow). The shared `levels` publication path adds the continuous bed, balances the
  stems and ending, then masters and checks the final AAC.

## Before and after (no AI)

For what you see rather than hear (foot IK, the gaze, the lean): the same bench scenario filmed twice,
the animation work off (OpenGothic as it is) and on, stacked top and bottom.

```bash
shorts/compare/film.sh bodies                                   # .shorts/compare/bodies/{off,on}/<segment>.mp4|.wav
uv run --project shorts python shorts/compare/cut.py bodies ru  # -> .shorts/compare/bodies/short-ru.mp4 (+ .png)
```

`bodies`: the animation work off and on. `archery`: the bow as Gothic II has it (the auto aim and the dice,
`OPENGOTHIC_MANUALAIM=0 OPENGOTHIC_NPCAIM=0`) and aimed by hand: the draw, a head shot from the game's
camera (`ui on`: the crosshair), the cant, steps with the bow drawn, leaning out round a corner, paladins
at a runner. `hits`: the body physics off (`off:` ragdoll, living bodies, arrow reactions, stuck arrows; our
flinch too) and on: three arrows stay in a bandit, one killed on the run, a bandit and a wolf killed on a slope
over the sea; filmed with `MUSIC=0`, a theme of the game under it. A line break in the hook makes it two lines.

`water`: three daytime scenes at Khorinis harbour: a farmer killed by the wind fist, a fireball
at a crate and barrel, and a sword, ore, lute and arrow dropped into deep water. Run
`shorts/compare/film.sh water` for both takes. `water-film setup body|fireball|things` sets a fixed close side
camera and a fresh scene; `water-film begin` starts its native action and `water-film checkpoint` logs the
outcome after six seconds. The off take disables ragdoll, living bodies, spell features, movable objects,
destruction and water after every setup, with `OPENGOTHIC_FLINCH=0`. The existing `water.bench` and
`water-bodies.bench` checks keep their own fixtures.
The cameras frame the impact and water surface closely; the stock corpse may leave the off frame during
its throw. At each captured frame, `water_film_framing` measures the pixel projection of rendered NPC bones
or the visual model bounds of an item or intact prop; it marks departures from the frame without aborting.

- `compare/<name>.bench`: the scenario; every `rec` is a beat. Both takes run the same steps on the fixed
  time step, so the halves stay in sync; the "off" take sets `OPENGOTHIC_FOOTIK=0`, `_GAZE`, `_LEAN`, `_AIM`,
  `_FLINCH`, `_ANIBLEND`, `_LIPSYNC` to 0, or what `"off_env"` of `compare/<name>.json` names. A line
  `off: <command>` / `on: <command>` runs in that take only (the two ways of doing the same thing, e.g. the
  auto aim shoots on action + forward, the hand lets go of the action key). Frames 1080x960 (`TEMPEST_WINDOW_SIZE`), the game's sound into
  `<segment>.wav` (`TEMPEST_AUDIO_LOOPBACK`, `SOUND`/`MUSIC` of `scripts/bench.sh`).
- `compare/<name>.json`: the hook, the two labels, the beats in order with their trims and titles, per language.
  Optional: a beat's `"slow": {"from", "to", "ru", "en"}` (seconds of the segment; a `slowmo` stretch of the
  scenario, silent in the takes) puts its mark in the corner of both halves; `"music": {"file", "volume"}` lays
  a bed under the whole short, the file under `.shorts/compare/<name>/` (cut from a theme of the game by the
  explainer's `bed.sh`, never in git).
- A voice: a beat's `"lines": [{"at", "file", "words": [[word, from, to], ...]}]` lays each voiced line at that
  second of the segment and captions it in place of the beat's title, a page of up to three words at a time with
  the word being said lit (the timings from `gothic-shorts align`); the game's sound and the bed duck under it.
  Subtitle tracks and dubbing use these lines' positions in the plan, including the duration of unvoiced beats.
  Such a plan (its script, the voice files) stays outside the repository, in the explainer's workspace: give its
  path as the third argument, the short goes into that folder's `render/`:
  `uv run --project shorts python shorts/compare/cut.py hits ru ~/.local/share/gothic-ai/explain/hits/plan.json`.
- The channel's brand, on by default: its end sting after the last beat (below); every text of the cut —
  labels, titles, captions, the hook — in PT Serif
  Bold (ParaType, OFL, ships with macOS: Cyrillic, bookish like the channel's letters) on warm dark bands. The
  shorts published before it (`bodies`, `archery`, `hits`) say `"brand": false` and cut as they went out: Arial
  and Georgia on black bands, no sting. They are not re-uploaded.
- `cut.py` stacks each beat, grades both halves the same (a little brighter), takes the sound of the "on"
  take (with the bed under it), folds each stem to dual mono before levelling, limits the peaks and brings
  it to -14 LUFS by one gain (loudnorm in two passes,
  linear: one pass lifts the quiet stretches), fading out over the last 80 ms. Parts and the sting use
  the same AAC encoder at 48 kHz stereo. The unvoiced master also checks decoded AAC with `dual_mono`
  and removes the output if L−R exceeds −90 dBFS.

### The brand: the end sting

`shorts/brand/sting.py` renders the end sting, 2.2 s of 1080×1920: a letter on the desk at night, GOTHIC AI written
in ink from left to right, the red wax seal with the G of that lettering pressed on (the frame jolts) to the game's
diary-entry sound, the candle flickering. Its pictures and sound are not in
the repository (generated art, a sound of the game): `~/.local/share/gothic-ai/brand/sting/` holds `bg.png` (the
blank letter, top-down), `seal-green.png` (the seal on chroma green, keyed out),
`lettering.png` (the name, traced once from a generated pen master, so it never changes) and `LOGENTRY.WAV`
(from the game's `Sounds.vdf`).

```bash
uv run --project shorts python shorts/brand/sting.py    # -> ~/.local/share/gothic-ai/brand/sting/sting.mp4
```

Pick spots in daylight and open ground (`clear` sends rats and wolves away), and look at both takes before
cutting: a difference that does not read at phone size (a flinch on top of the game's stumble) stays out.
Scout on Ryzen (`OPENGOTHIC_BENCH_TAKE=off|on BENCH_HOST=ryzen scripts/bench.sh compare/<name>.bench <dir>`,
silent) and film on the Mac. A camera put by hand near a target on a slope or stairs often lands in ferns:
`foreach slope|stairs <n>` with `views watched 1 1000 shoot <prefix>` gives a still of each spot from a camera
that sees both; `view clean` clips the ground in front of the camera too (white wedges).

## Word timings of a known text

For captions that follow the voice word by word: the recording and exactly what it says go to the GPU
server, the forced aligner there (Qwen3 Forced Aligner 0.6B on audio.cpp, `scripts/deploy-gpu.sh aligner`
installs it; nothing runs on the Mac but ffmpeg) maps the words onto the sound.

```bash
uv run gothic-shorts align voiceover.wav "Ксардас, меня прислал Ли."     # -> voiceover.words.json
uv run gothic-shorts align voiceover.m4a --text-file voiceover.txt --out beat-1   # -> beat-1.json
```

`{"text", "duration", "words": [{"text", "start", "end"}]}`, seconds of the recording. The words keep the
text's punctuation ("Ли." rather than "Ли") when the aligner's words match the text one to one. The text must
be what is said, word for word (numbers as spoken); `--language` defaults to Russian (`ru` works the same).
The timings come in steps of 80 ms (the model's grid). GOTHIC_GPU_SSH comes from the environment or the
nearest `.env`, as for `scripts/deploy-gpu.sh`. A line of 5 s takes ~3 s from here: 0.1 s of alignment, ~1 s
loading the model (1.5 GB of GPU memory while it runs), the rest ssh and scp; the server's copies are deleted.

## Recording a voice-over

A studio in the browser for the voice of an explainer (`explain/`): the script as a teleprompter, the
short's draft playing the part each line is said over, a take per press of the space bar.

```bash
uv run gothic-shorts studio dice          # ~/.local/share/gothic-ai/explain/dice; opens http://127.0.0.1:47871
```

The short's folder holds `script.ru.txt` (a line of the voice per row), a plan with the lines' places
(`plan.json` or `plan.storyboard.json`) and the draft `render/preview.mp4` (without captions). Space starts a
take: the picture runs from 1.5 s before the line's place, the line lights up when its place comes, a bar
shows how long the plan gives it; Space again keeps the take, Esc throws it away. Enter plays the chosen take
with the picture, the arrows change the line, 1-9 choose a take. Press Backspace (or the "В корзину" button)
twice within 3 s to send the chosen take to `voice/trash/`; Esc cancels the pending discard. Z (or "Вернуть дубль")
restores the last discarded take from `voice/trash/` and selects it, returning to its line. The undo stack lasts
until the page is reloaded; Z also works with the Russian keyboard layout.
The trash and restore buttons stop playback or rehearsal before acting.
Preparing a recording keeps its line fixed; restoring a take waits for earlier take selections to finish.
"Тишина 10 с" records the room for the noise reduction, "Прогон" runs the draft with the prompter alone.

The microphone is taken as it is (no echo cancelling, noise suppression or automatic gain) and the take is
sent as a lossless WAV: `voice/raw/03-2.wav` is take 2 of line 3, 48 kHz mono 24-bit, next to its
`03-2.json` (length, peak, where the speech starts and ends, where the picture was); `voice/takes.json`
keeps the chosen ones. Embedded browsers (an app's preview pane) may refuse the microphone: open the address
in Chrome or Safari. Headphones if the game's sound is on, or it ends up in the take.

Then the chosen takes become the short's voice:

```bash
uv run gothic-shorts words dice           # -> voice/clean/NN.wav, words.ru.json
```

Each take is cleaned (low cut, de-esser, gentle compression, the silence at both ends trimmed to a breath) and
aligned on the GPU server, and the lines are laid one after another with 0.35 s between them
(`voice/gaps.json`, e.g. `{"4": 0.7}`, gives a line a longer pause after it). `words.ru.json` has every word's
time on the short's timeline, for the plan's captions and for placing the picture on the words. The captions
follow `script.ru.said.txt` when it exists: what was really said, where it differs from the script. A line whose
take, text and cleaning have not changed is not aligned again.

## The English sound track

```bash
scripts/deploy-gpu.sh dub                 # once, from the repository: VoxCPM voice-cloning job
uv run gothic-shorts dub xardas-lie       # a shot's newest take (or `dub xardas-lie 2`) -> <take>/dub.en.wav
uv run gothic-shorts dub bow              # an explainer -> <workspace>/render/dub.en.wav
uv run gothic-shorts dub bow --reclone    # the user's kept voice made anew from bow's recordings
```

YouTube offers a video's sound in several languages (Studio: Subtitles, the video, the language's Audio
column; the API has no call for it, and the channel needs YouTube's advanced features). Publish the Russian
short with `gothic-shorts cut` or `gothic-shorts explain` first: `short-ru.mp4` and a successful
`short-ru.levels.json` (`"ok": true`) must be beside the dub's outputs. The current shot or plan must still
have the published short's duration (within one frame at 30 fps).

`dub` replaces the Russian voice stem with English and publishes through the same `levels.publish` as the
Russian short: voice −14 LUFS, true peak at most −1 dBTP after AAC, voice above effects above music, the
ending at S−6, and no clicks. English stems are folded to dual mono before levelling; the `dual_mono`
gate checks the decoded English AAC for max L−R ≤ −90 dBFS. It writes `dub.en.levels.json` with the checks
and prints the levels table,
each line's timing, speed-up and gain, and both languages' mastered voice levels. `dub.en.mp4` copies the
Russian picture without re-encoding, with the verified English AAC first (default, `eng`) and the original
Russian audio second (`rus`), with faststart. Published English AAC has two identical channels.
`dub.en.wav` decodes that English AAC to 48 kHz stereo dual mono, 24-bit PCM, exactly as long as the Russian
short's audio (within 1 ms). Input-check refusals leave previous
dub files untouched; old outputs are removed only when assembly starts, after which failures leave neither
WAV nor MP4, while a failed levels report is kept.

- **A shot's take.** Effects, the brand ending and the music bed are built by the same code as the Russian
  cut, with the same removed AI pauses. The recording must have separate `npc_voice` and have been made
  with `musicVolume=0` and `worldSounds=0`, as `cut` requires; older takes must be recorded again. No voice
  separation or GPU job is needed for the sound stems. Every line in English: the take's `subs.en.json`
  (what the captions say; the hook and captions stay burnt in in Russian), in the voice the game speaks
  Russian in: the TTS service voices a Russian
  sample (~17 s) in the npc's voice (its id and description from the AI server, `gothic-ai voices <npc>`;
  the hero's the shot's `hero_voice`), Inworld clones it (Instant Voice Cloning, `gothic-tts say --clone`),
  and the clone says the English lines, so the timbre stays the Russian one. When that fails Inworld
  designs an en-US voice from the description, and then the launcher's fallback takes over, as in the
  game: VoxCPM2 on the GPU server designs one reading an English sample (as the voxcpm bridge does with
  its Russian one) and says the lines in it (`--tts voxcpm` picks it). A fallback says why the way before
  it failed. Each line starts where its Russian one starts; all English lines of each speaker are levelled
  together with `stems.speaker_gains`, with short fades at their edges, as in the Russian voice stem.
  The lines are kept by voice and text in `.shorts/voice/en/`, the voices in the kept voices (below) and
  Inworld's library (`gothic-ai <id>@en~ru`). The lip sync stays the Russian one.
- **An explainer.** `script.en.txt` (the caption track's text, without its `|`) in the user's own voice: by
  Inworld, the clone `USER` of the kept recording `user.inworld.wav` (20-29 s of the user's lines; its best
  is 10-30 s), or, when Inworld fails, by VoxCPM2 on the GPU server from `user.voxcpm.wav` (8-14 s: its
  encoder takes up to 15 s); cleaned as the Russian takes were (`words.py`'s chain), each line from where
  its Russian line's first word is said (`words.ru.json`). Remotion renders only the `game` and `music`
  stems from the Russian plan (`plan.json`, `--plan` for another); the picture comes from the published
  short, and the English voice replaces its Russian voice stem. Footage sound (`gameVolume` above 0) is
  supported. English lines are levelled together as one speaker, and `levels.publish` balances the stems
  using the English speech windows and the Russian publication's music fades. The lines live in `voice/en/`
  and are made again only when the voice or a text changes.

The voices are kept for every short and every checkout in `~/.local/share/gothic-ai/voices/`, so each is
cloned or designed once: `user.inworld.wav` and `user.voxcpm.wav` (the user's recorded voice, made from
the first explainer dubbed without them: its first lines in `voice/clean/`), `<npc>-<key>.ru.wav` (the
Russian sample Inworld clones an npc's English voice from), `<npc>-<key>.ref.wav` (a voice VoxCPM
designed). The clone is replaced only on demand: `dub <explainer> --reclone` makes both recordings anew
from that explainer's lines (Inworld clones it again, every explainer's English lines are voiced again on
their next dub), as deleting a file does for that one. Inworld keeps its clones in the workspace's library
and the TTS service's `~/.local/share/gothic-ai/inworld-voices.json` maps the id (`USER`) to the clone of
the recording it was made of.

A line longer than its slot (until the next line starts, 0.15 s before it) is sped up by up to x1.15 (pitch
kept); one longer still is reported, and `dub` exits with 1 after writing the track: shorten the line in
the translation and run it again. Inworld and VoxCPM read English at about 12-14 characters a second.
Inworld bills the lines and the samples: check its credits first (`gothic-launcher` does).

The voice-cloning job (`shorts/deploy/dub`, in `~/gothic-ai/dub` on the server) is
`clone.sh <reference> <lines.json> <out dir> [--design <json>]`. It uses only Python's standard library and
the engine of the running gothic-voxcpm, with no GPU memory of its own; the game's lines wait their turn
meanwhile. Files go over by scp into a fresh folder there and it is deleted after.

## Covers

Template B is a 1080×1920 JPEG (at most 2 MB): a dark band with soft 60 px edges at the seam of the
two takes, an uppercase white topic in Arial Black and an optional gold second line, with a dark outline
and blurred shadow. It needs macOS's `/System/Library/Fonts/Supplemental/Arial Black.ttf`.
Both lines stay in the centre square (y 420–1500), also safe in the channel grid's centre 2:3 crop.

A comparison plan can select its clean cover frame, in seconds of the recorded segment:

```json
"cover": {
  "segment": "tavern", "at": 1.2,
  "title": {"ru": "МЕБЕЛЬ", "en": "FURNITURE"},
  "sub": {"ru": "БЫЛО → СТАЛО", "en": "BEFORE → AFTER"}
}
```

`sub` is optional and defaults to those two translations. `compare/cut.py` stacks off/on frames at `at`
with the cut's `GRADE`, without its labels or subtitles, and writes `cover-<lang>.jpg` beside
`short-<lang>.mp4`. Without `cover` it only prints that no cover was requested.
For AI shorts and explainers, use a video frame or an image (centre cropped to 1080×1920):

```bash
uv run gothic-shorts cover path/to/short-ru.mp4 --at 1.8 --title "МАГИЯ" --sub "БЫЛО → СТАЛО" --out cover-ru.jpg
uv run gothic-shorts cover frame.png --title "МЕБЕЛЬ"  # -> cover.jpg beside frame.png, no second line
uv run gothic-shorts upload path/to/short-ru.mp4 --meta path/to/youtube.json --thumbnail cover-ru.jpg
uv run gothic-shorts thumbnail <video-id> cover-ru.jpg  # asks first; --yes skips confirmation
```

`--thumbnail` sets the cover after uploading; `thumbnail` sets it on an existing video. Both check the
channel as `upload` does. The request uses `thumbnails.set`, `uploadType=media`, `image/jpeg`; the existing
`youtube.upload` scope is enough. If YouTube rejects a cover during `upload`, the video stays up and the command
prints the error and a reminder to choose a frame in the YouTube app.

## Uploading to YouTube

```bash
uv run gothic-shorts youtube login                     # once: the browser opens Google's consent page
uv run gothic-shorts upload .shorts/compare/bodies/short-ru.mp4 --meta .shorts/compare/bodies/youtube.json --privacy public
uv run gothic-shorts upload .shorts/xardas-lie/take-02/short-ru.mp4 --meta .shorts/xardas-lie/youtube.json \
    --captions en=.shorts/xardas-lie/take-02/subs.en.srt
uv run gothic-shorts srt bow                           # an explainer: subs.en.srt from script.en.txt
uv run gothic-shorts captions <video id> subs.en.srt --lang en   # a track on a video already up
uv run gothic-shorts translate <video id> --meta youtube.json    # its other languages on a video already up
```

A short is one video in every language, not one upload per language: its sound is Russian (the game is;
`defaultAudioLanguage` is always `ru`), the `--lang` section of `youtube.json` (`ru`) gives the title and the
description, every other section goes in as their translation (YouTube shows a viewer the title in their
language), and `--captions <lang>=<file>` adds subtitle tracks after the upload. `gothic-shorts srt` makes
them: a shot's take from its `subs.<lang>.json` on the cut's time (an npc's line starts with its name, the
burnt-in hook and captions in brackets), an explainer from `script.<lang>.txt` over its Russian voice
(`words.ru.json`, the `|` chunks one after another). For comparisons, subtitles and dubbing follow the voiced
lines in `plan.json` (`srt --plan FILE` selects another plan); `words.ru.json` is not needed. Caption tracks
need the `youtube.force-ssl` scope: a sign-in older than it can still upload, but `--captions` asks to `youtube login` again before it sends
anything. `translate` adds the other sections' titles and descriptions to a video already up (videos.update,
the same scope): its own title, description, tags and other translations stay as they are. An English audio track (`gothic-shorts dub`) is added in Studio (Subtitles, the video, the
language's Audio column); the API has no call for it, and Studio shows the column only to channels with
YouTube's advanced features.

The OAuth client is a Desktop app client of a Google Cloud project (YouTube Data API v3); its JSON and the
token live in `~/.local/share/gothic-ai/` (`youtube-client.json`, `youtube-token.json`), never in the
repository. Publish the app (Google Auth Platform, Audience, Publish app): a testing app's token lasts 7 days,
a published one's does not, and Google's verification is not needed for the channel's own account (the
consent page warns that the app is not verified: Advanced, go on). `login` signs in again and says how long
the token lasts. The metadata live next to the short, outside the
repository (`.shorts/<name>/youtube.json`), one section per language: title (up to 100 characters),
description, tags, `synthetic` (altered or synthetic content: set it for shorts with AI voices). Uploads are
private unless `--privacy public` (or `unlisted`). YouTube no longer locks the uploads of a project without
its audit to private, and `upload` reads the status back: if YouTube kept the video private, it says so (then
the project needs YouTube's API audit, and Studio can not change it). `upload` shows the channel, the file and the title and asks before it sends anything; it refuses any
channel but the project's (GothicAI, `UCbeAoPh3V7yFVpiuWNI-wkw`; `--channel` or `GOTHIC_YOUTUBE_CHANNEL`).

## Plan and log

- 2026-10-22: fold audio stems to dual mono before levelling and measurements; publish identical AAC
  channels and reject voiced publication when the decoded `dual_mono` gate exceeds −90 dBFS L−R.
- 2026-10-22: disable PNS and intensity stereo and force M/S in the 256k AAC fallback; use the shared
  encoder for comparison parts and stings, and check unvoiced comparison AAC with `dual_mono`.

