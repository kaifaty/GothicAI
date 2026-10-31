import {z} from 'zod';
import {PUSH_MAX, TRANSITION_DUR} from './parts/timing.ts';

// The plan of a short (plan.json in the short's workspace). Times are seconds: `at`, `until` and keyframes from the
// start of their beat, words and voice lines from the start of the short. A missing field fails the render.

const Text = z.object({
  text: z.string(),
  at: z.number().default(0),
  until: z.number().optional(),
  y: z.number().optional(), // top in frame pixels; defaults to 260 tall / 140 wide
  size: z.number().default(64),
  color: z.string().optional(),
  enter: z.enum(['rise', 'pop', 'slide']).default('rise'), // parts/motion.ts
});

const TagLine = z.object({
  text: z.string(), // `{n}` is replaced by the counter
  at: z.number().default(0),
  color: z.string().optional(),
  count: z.object({to: z.number(), dur: z.number().default(0.8)}).optional(), // a roll that runs and stops
});

// a label over a thing in the picture; x, y: pixels of the selected frame, from the bench's data
const Tag = z.object({
  x: z.number(),
  y: z.number(),
  lines: z.array(TagLine).min(1),
  at: z.number().default(0),
  until: z.number().optional(),
  side: z.enum(['center', 'right', 'left']).default('center'), // where the box opens from the pin
  enter: z.enum(['rise', 'pop', 'slide']).default('rise'), // parts/motion.ts
});

const Point = z.tuple([z.number(), z.number()]); // pixels of the selected frame, from the bench's data

// a path that draws itself (an arrow's flight, a route): through `points`, smoothed unless `curve` is false; its
// tip runs from the first point to the last over `dur`, an arrowhead on it with `arrow`
const PathLine = z.object({
  points: z.array(Point).min(2),
  curve: z.boolean().default(true),
  color: z.string(),
  at: z.number().default(0),
  dur: z.number().default(0.6),
  until: z.number().optional(),
  width: z.number().default(8),
  arrow: z.boolean().default(false),
  dash: z.boolean().default(false),
});

// a line over the picture (a line of sight from the bench's `sight`): it draws itself from `from` toward `to`; with
// `cut` it stops there (a wall in the way): solid up to it, `cutColor` and dashed past it from `cutAt`, a cross on it
const Stroke = z.object({
  from: Point,
  to: Point,
  cut: Point.optional(),
  color: z.string(),
  cutColor: z.string().optional(),
  at: z.number().default(0),
  dur: z.number().default(0.6),
  cutAt: z.number().optional(),
  until: z.number().optional(),
  width: z.number().default(8),
});

// a fan from a point (the bow's focus from the head): it opens from the edge toward `a` to the edge toward `b`, as
// long as the one toward `b`
const Fan = z.object({
  apex: Point,
  a: Point,
  b: Point,
  color: z.string(),
  at: z.number().default(0),
  dur: z.number().default(0.8),
  until: z.number().optional(),
});

// one thing carried across the cut into this beat (parts/Carry.tsx): a line of a tag of the beat before (or a point
// of its picture with `text`) flies along an arc to `to`, the next thing in the chain lands where it lands. It
// starts on the cut (`at` 0) and takes `dur`, by default as long as its travel asks (parts/timing.ts travelDur)
const Carry = z.object({
  from: z.union([Point, z.object({tag: z.number().int().nonnegative(), line: z.number().int().nonnegative().default(0)})]),
  to: Point, // frame pixels of this beat: where the centre of the text lands
  text: z.string().optional(), // what flies; default: the tag line's text as it stands at the cut
  at: z.number().nonnegative().default(0),
  dur: z.number().positive().optional(),
  size: z.number().positive().optional(), // the font size on landing; it flies from the tag's 52
  color: z.string().optional(), // default: the tag line's colour
  arc: z.number().default(0.15), // the bend as a share of the distance; positive bows upward, 0 is straight
  until: z.number().optional(),
});

const CurveKind = z.enum(['exe', 'opengothic']);

const DiceChart = z.object({
  type: z.literal('dice'),
  talent: z.number(),
  axisX: z.string(),
  axisY: z.string(),
  source: z.string(), // small print in the corner
  // each curve is drawn up to the distance its keyframes give at the time: [[t, metres], ...]
  curves: z.array(z.object({curve: CurveKind, name: z.string(), draw: z.array(z.tuple([z.number(), z.number()]))})),
  talentMark: z.object({at: z.number(), text: z.string()}).optional(), // "навык": the talent's level at 15 m
  stepMark: z.object({at: z.number(), text: z.string()}).optional(), // "−⅓" at the step
  markers: z.array(z.object({d: z.number(), curve: CurveKind, at: z.number(), text: z.string()})).default([]), // `{v}`: the value
});

// the damage formula, built term by term, then a worked example computed from its inputs
const FormulaCard = z.object({
  type: z.literal('formula'),
  decimal: z.string().default(','), // the decimal mark of the language
  title: z.string(), // "УРОН ="
  terms: z.array(z.object({text: z.string(), at: z.number(), color: z.string().optional()})), // a line each
  legend: z.object({text: z.string(), at: z.number()}).optional(),
  example: z
    .object({
      at: z.number(),
      bow: z.number(),
      dex: z.number(),
      draw: z.number(),
      prot: z.number(),
      zones: z.array(z.object({name: z.string(), k: z.number(), at: z.number()})), // a result line each
    })
    .optional(),
  source: z.string(),
});

// bars by groups (e.g. damage per second by hand against the dice at 10 / 20 / 30 m); 1 = the reference
const BarsCard = z.object({
  type: z.literal('bars'),
  title: z.string(),
  series: z.array(z.object({name: z.string(), color: z.string()})),
  groups: z.array(z.object({
    label: z.string(), values: z.array(z.number()), at: z.number(),
    color: z.string().optional(), // overrides the series colour within this group
    note: z.string().optional(), // small print below the group label
  })),
  max: z.number(),
  log: z.boolean().default(false), // logarithmic height; zero stays on the baseline
  format: z.string().default('×{v}'), // how a value is written over its bar
  decimal: z.string().default(','),
  source: z.string(),
});

// marks filling one by one (e.g. 17 arrows of 30 in the head)
const DotsCard = z.object({
  type: z.literal('dots'),
  total: z.number(),
  hits: z.number(),
  at: z.number(),
  dur: z.number(),
  title: z.string(),
  caption: z.string(), // `{n}`: the counter
  source: z.string(),
});

// the character's numbers greying out, a question over them
const StatsCard = z.object({
  type: z.literal('stats'),
  rows: z.array(z.object({name: z.string(), value: z.string(), color: z.string().optional()})),
  fadeAt: z.number(),
  question: z.object({text: z.string(), at: z.number()}),
});

const NumberValue = z.number().finite();
const At = NumberValue.nonnegative();
const Axis = z.object({
  min: NumberValue,
  max: NumberValue,
  ticks: z.array(NumberValue).min(1),
  label: z.string(),
});

const LineCard = z.object({
  type: z.literal('line'),
  title: z.string(),
  axisX: Axis,
  axisY: Axis,
  series: z.array(z.object({
    name: z.string(), color: z.string(),
    points: z.array(z.tuple([NumberValue, NumberValue])).min(2),
    draw: z.tuple([At, NumberValue.positive()]), // [at, duration], revealed from left to right
  })).min(1),
  marks: z.array(z.object({x: NumberValue, text: z.string(), at: At})).default([]),
  labels: z.array(z.object({x: NumberValue, y: NumberValue, text: z.string(), at: At})).default([]),
  source: z.string(),
});

const BigCard = z.object({
  type: z.literal('big'),
  title: z.string().optional(),
  items: z.array(z.object({
    value: z.union([z.string(), NumberValue]), label: z.string(), at: At, color: z.string().optional(),
    count: z.object({
      from: NumberValue, to: NumberValue, dur: NumberValue.positive(),
      format: z.string().regex(/\{v(?::[1-6])?\}/, 'format должен содержать {v} или {v:1}…{v:6}'),
    }).optional(),
  })).min(1).max(3),
  source: z.string(),
});

const TableCard = z.object({
  type: z.literal('table'),
  title: z.string().optional(),
  columns: z.array(z.string()).min(1),
  rows: z.array(z.object({cells: z.array(z.union([z.string(), NumberValue])), at: At})).min(1),
  highlight: z.number().int().nonnegative().optional(), // zero-based column index
  source: z.string(),
});

const CellsCard = z.object({
  type: z.literal('cells'),
  title: z.string().optional(),
  cells: z.array(z.object({label: z.string(), state: z.enum(['fail', 'pass', 'skip']), at: At})).min(1),
  spans: z.array(z.object({from: z.number().int().nonnegative(), to: z.number().int().nonnegative(), text: z.string()})).default([]),
  notes: z.array(z.object({text: z.string(), at: At, color: z.string().optional()})).default([]),
  source: z.string(),
});

// Top-down observations in centimetres; x points right, y points forward (up on the card).
const RaysCard = z.object({
  type: z.literal('rays'),
  title: z.string().optional(),
  maxCm: NumberValue.positive(),
  rays: z.array(z.object({cm: NumberValue.nonnegative(), hit: z.boolean()})).length(16),
  target: z.object({x: NumberValue, y: NumberValue, label: z.string(), at: At.default(0)}),
  hideAt: At.optional(),
  panels: z.array(z.object({title: z.string(), lines: z.array(z.string()), color: z.string(), at: At})),
  raysAt: z.tuple([At, NumberValue.positive()]),
  source: z.string(),
});

const ConeCard = z.object({
  type: z.literal('cone'),
  title: z.string().optional(),
  halfDeg: NumberValue.positive().lt(180),
  rangeCm: NumberValue.positive(),
  path: z.array(z.object({t: At, x: NumberValue, y: NumberValue})).min(1),
  targetLabel: z.string().default('противник'),
  turn: z.object({at: At, dur: NumberValue.positive(), deg: NumberValue, label: z.string()}).optional(),
  texts: z.array(z.object({text: z.string(), at: At, color: z.string()})).default([]),
  source: z.string(),
});

const TicksCard = z.object({
  type: z.literal('ticks'),
  title: z.string().optional(),
  rows: z.array(z.object({
    label: z.string(), cells: z.string().regex(/^[DB]+$/, 'cells должны содержать только D и B'),
    offset: NumberValue.nonnegative().default(0), at: At,
  })).min(1),
  dur: NumberValue.positive(),
  legend: z.object({D: z.string(), B: z.string()}),
  align: z.object({at: At, dur: NumberValue.positive()}).optional(),
  source: z.string(),
});

const GridCard = z.object({
  type: z.literal('grid'),
  title: z.string(),
  tiles: z.array(z.object({label: z.string(), at: At})).min(1),
  columns: z.number().int().positive(),
  source: z.string(),
});

const FlowCard = z.object({
  type: z.literal('flow'),
  title: z.string().optional(),
  nodes: z.array(z.object({
    id: z.string(), label: z.string(), x: NumberValue.min(0).max(1), y: NumberValue.min(0).max(1),
    w: NumberValue.positive().max(1).optional(), color: z.string().optional(), at: At,
  })).min(1),
  edges: z.array(z.object({
    from: z.string(), to: z.string(), label: z.string().optional(), at: At,
    curve: NumberValue.optional(), // signed bend, as a fraction of the frame height
    packets: z.object({every: NumberValue.positive(), dur: NumberValue.positive()}).optional(),
  })),
  source: z.string(),
});

const Chart = z.discriminatedUnion('type', [DiceChart, FormulaCard, BarsCard, DotsCard, StatsCard, LineCard, BigCard, TableCard, CellsCard, RaysCard, ConeCard, TicksCard, GridCard, FlowCard]);

const Follow = z.object({
  data: z.string().min(1), // actions.json, relative to the workspace's public dir
  names: z.record(z.string()), // only named actions are shown
  at: At.default(0),
  until: At.optional(),
  offsetY: NumberValue.default(-42), // display pixels from the head; negative is above it
  hold: NumberValue.positive().default(1.2),
});

const Hp = z.object({
  data: z.string().min(1),
  labels: z.tuple([z.string(), z.string()]),
  at: At.default(0),
});

// Recorded footage pixels and frames, before cover/zoom. Null means the fighter is off camera.
export const CombatData = z.object({
  fps: NumberValue.positive(),
  frames: z.number().int().positive(),
  size: z.tuple([NumberValue.positive(), NumberValue.positive()]),
  track: z.array(z.union([
    z.tuple([At, NumberValue, NumberValue]),
    z.tuple([At, z.null(), z.null()]),
  ])),
  hp: z.array(z.tuple([At, NumberValue, NumberValue])),
  actions: z.array(z.object({frame: At, t: At, action: z.string(), id: z.number().int()})),
});

const Beat = z.object({
  id: z.string(),
  kind: z.enum(['footage', 'still', 'card', 'split']),
  dur: z.number().positive(),
  src: z.string().optional(), // footage mp4 or a still png in the workspace (the public dir)
  from: z.number().default(0), // where the footage starts
  srcBottom: z.string().optional(), // split: bottom in tall, right in wide
  fromBottom: z.number().default(0),
  labels: z.tuple([z.string(), z.string()]).optional(), // split: top/bottom in tall, left/right in wide
  labelColors: z.tuple([z.string(), z.string()]).optional(), // default: the old rule's grey, the new one's gold
  freeze: z.number().optional(), // footage stops here (seconds into the beat)
  zoom: z.object({at: z.number(), dur: z.number(), scale: z.number(), x: z.number(), y: z.number(),
    ease: z.enum(['inOut', 'linear']).default('inOut')}).optional(), // linear: a slow push across the beat
  // a held picture of 2 s or more pushes in by itself (parts/timing.ts holdPush): 0 turns it off, 0.03-0.05 sets it
  push: z.number().min(0).max(PUSH_MAX).optional(),
  // how this beat comes in; the beat before it stays under it for `dur` (parts/motion.ts), so no time moves.
  // Without `dur`: 0.5 s for a slide or wipe (the whole frame travels), 0.35 s for the rest (parts/timing.ts)
  enter: z.object({type: z.enum(['fade', 'slide', 'wipe', 'zoom', 'flash']), dur: z.number().positive().optional()})
    .transform((e) => ({type: e.type, dur: e.dur ?? TRANSITION_DUR[e.type]})).optional(),
  game: z.number().default(1), // the beat's relative game level, also kept in the game stem
  texts: z.array(Text).default([]),
  tags: z.array(Tag).default([]),
  strokes: z.array(Stroke).default([]),
  fans: z.array(Fan).default([]),
  paths: z.array(PathLine).default([]),
  follow: Follow.optional(),
  followBottom: Follow.optional(), // split: bottom in tall, right in wide
  hp: Hp.optional(),
  hpBottom: Hp.optional(),
  chart: Chart.optional(),
  carry: Carry.optional(),
});

const Word = z.object({text: z.string(), start: z.number(), end: z.number()});

export const Plan = z.object({
  format: z.enum(['tall', 'wide']).default('tall'),
  stem: z.enum(['voice', 'game', 'music']).optional(), // uncalibrated sources for publication through gothic_shorts.levels
  brand: z.boolean().default(true),
  brandVolume: z.number().nonnegative().default(0.355), // draft mix only; levels sets the published sting level
  lang: z.enum(['ru', 'en']),
  hook: z.object({text: z.string(), until: z.number()}),
  beats: z.array(Beat).min(1),
  lines: z.array(z.array(Word)).default([]), // the captions: the voice's words, a sentence per line
  voice: z.array(z.object({file: z.string(), start: z.number()})).default([]),
  duck: z.number().default(0.3), // draft game ducking; ignored by publication through levels
  gameVolume: z.number().default(1), // draft footage volume (0: none); levels calibrates the published game stem
  // one music bed under the beats; publication keeps its fades and loop but levels sets its level and ducking
  music: z
    .object({
      file: z.string(),
      volume: z.number(), // draft level in the voice's pauses
      duck: z.number().default(0.5), // draft multiplier while the voice speaks
      fadeIn: z.number().default(1),
      fadeOut: z.number().default(1.5),
    })
    .optional(),
}).superRefine((plan, ctx) => {
  plan.beats.forEach((beat, bi) => {
    if (beat.push && beat.zoom) {
      ctx.addIssue({code: z.ZodIssueCode.custom, path: ['beats', bi, 'push'], message: 'push и zoom в одном бите: оставь один'});
    }
    const carry = beat.carry;
    if (carry) {
      const issue = (path: (string | number)[], message: string) =>
        ctx.addIssue({code: z.ZodIssueCode.custom, path: ['beats', bi, 'carry', ...path], message});
      const prev = plan.beats[bi - 1];
      if (!prev) issue([], 'carry: нужен бит перед этим');
      else if (!Array.isArray(carry.from)) {
        const tag = prev.tags[carry.from.tag];
        if (!tag) issue(['from', 'tag'], 'нет такой метки в бите перед этим');
        else if (!tag.lines[carry.from.line]) issue(['from', 'line'], 'нет такой строки в метке');
        else if (tag.lines[carry.from.line].at >= prev.dur) issue(['from', 'line'], 'строка метки не успевает появиться до склейки');
        else if (tag.until !== undefined && tag.until < prev.dur) issue(['from', 'tag'], 'метка уходит до склейки');
      } else if (carry.text === undefined) issue(['text'], 'carry из точки: нужен text');
    }
    const chart = beat.chart;
    if (!chart) return;
    const issue = (path: (string | number)[], message: string) =>
      ctx.addIssue({code: z.ZodIssueCode.custom, path: ['beats', bi, 'chart', ...path], message});
    if (['line', 'big', 'table', 'cells', 'rays', 'cone', 'ticks', 'grid', 'flow'].includes(chart.type) && plan.format !== 'wide') {
      issue(['type'], `${chart.type}: только wide`);
    }
    if (chart.type === 'line') {
      for (const axis of ['axisX', 'axisY'] as const) {
        const a = chart[axis];
        if (a.max <= a.min) issue([axis, 'max'], 'max должен быть больше min');
        a.ticks.forEach((v, i) => {
          if (v < a.min || v > a.max) issue([axis, 'ticks', i], 'деление вне диапазона оси');
        });
      }
      chart.series.forEach((s, si) => s.points.forEach(([x, y], pi) => {
        if (pi > 0 && x <= s.points[pi - 1][0]) issue(['series', si, 'points', pi, 0], 'x должны строго возрастать');
        if (x < chart.axisX.min || x > chart.axisX.max) issue(['series', si, 'points', pi, 0], 'точка вне диапазона X');
        if (y < chart.axisY.min || y > chart.axisY.max) issue(['series', si, 'points', pi, 1], 'точка вне диапазона Y');
      }));
    } else if (chart.type === 'table') {
      if (chart.highlight !== undefined && chart.highlight >= chart.columns.length) issue(['highlight'], 'индекс вне columns');
      chart.rows.forEach((row, i) => {
        if (row.cells.length !== chart.columns.length) issue(['rows', i, 'cells'], 'число ячеек должно совпадать с columns');
      });
    } else if (chart.type === 'cells') {
      chart.spans.forEach((span, i) => {
        if (span.to < span.from || span.to >= chart.cells.length) issue(['spans', i, 'to'], 'диапазон вне cells: from ≤ to < cells.length');
      });
    } else if (chart.type === 'rays') {
      chart.rays.forEach((ray, i) => {
        if (ray.cm > chart.maxCm) issue(['rays', i, 'cm'], 'длина луча должна быть ≤ maxCm');
      });
    } else if (chart.type === 'cone') {
      chart.path.forEach((point, i) => {
        if (i > 0 && point.t <= chart.path[i - 1].t) issue(['path', i, 't'], 't должны строго возрастать');
      });
    } else if (chart.type === 'flow') {
      const ids = new Set<string>();
      chart.nodes.forEach((node, i) => {
        if (ids.has(node.id)) issue(['nodes', i, 'id'], 'id узлов должны быть уникальными');
        ids.add(node.id);
      });
      chart.edges.forEach((edge, i) => {
        for (const endpoint of ['from', 'to'] as const) {
          if (!ids.has(edge[endpoint])) issue(['edges', i, endpoint], 'ссылка на неизвестный узел');
        }
      });
    } else if (chart.type === 'bars' && chart.log) {
      if (!Number.isFinite(chart.max) || chart.max <= 0) issue(['max'], 'для log нужен max > 0');
      chart.groups.forEach((g, gi) => g.values.forEach((v, vi) => {
        if (!Number.isFinite(v) || v < 0 || v > chart.max) issue(['groups', gi, 'values', vi], 'для log нужно 0 ≤ value ≤ max');
      }));
    }
  });
});

export type Plan = z.infer<typeof Plan>;
export type CombatData = z.infer<typeof CombatData>;
export type Follow = z.infer<typeof Follow>;
export type Hp = z.infer<typeof Hp>;
export type Beat = z.infer<typeof Beat>;
export type DiceChart = z.infer<typeof DiceChart>;
export type FormulaCard = z.infer<typeof FormulaCard>;
export type BarsCard = z.infer<typeof BarsCard>;
export type DotsCard = z.infer<typeof DotsCard>;
export type StatsCard = z.infer<typeof StatsCard>;
export type LineCard = z.infer<typeof LineCard>;
export type BigCard = z.infer<typeof BigCard>;
export type TableCard = z.infer<typeof TableCard>;
export type CellsCard = z.infer<typeof CellsCard>;
export type RaysCard = z.infer<typeof RaysCard>;
export type ConeCard = z.infer<typeof ConeCard>;
export type TicksCard = z.infer<typeof TicksCard>;
export type GridCard = z.infer<typeof GridCard>;
export type FlowCard = z.infer<typeof FlowCard>;
export type TagT = z.infer<typeof Tag>;
export type CarryT = z.infer<typeof Carry>;
export type StrokeT = z.infer<typeof Stroke>;
export type FanT = z.infer<typeof Fan>;
export type PathT = z.infer<typeof PathLine>;
export type TextT = z.infer<typeof Text>;
export type Word = z.infer<typeof Word>;
