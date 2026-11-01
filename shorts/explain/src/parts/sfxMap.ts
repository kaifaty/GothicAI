// The sound of the motion: which sound where, from the plan alone, with the timings the drawing uses. No Remotion
// here, so the tests and scripts/sfxmap.mjs read the same map that Sfx.tsx plays.
//
// Only what matters gets a sound (the user, 2026-11-01): a card coming in, a frame sliding in, a label flying, a
// counter running and stopping, a key number or verdict landing, a step down, a punch-in. Captions, the hook, plain
// cuts and ordinary label openings stay silent. The sounds are a "scriptorium" (paper, books, wood): sfx.json.

import type {Beat, Plan} from '../schema';
import {lineAt} from './carryMath.ts';
import {MOTION, travelDur} from './timing.ts';

// lead: where a variant's peak sits in its file (sfx.json, scripts/sfx_lib.py), so it starts that much before its
// frame; gain: the kind's level in the effects (all variants are made equally loud); rank: which one stays when two
// come too close
export const KINDS = {
  page: {lead: 0.25, variants: 5, gain: 0.45, rank: 5},
  punch: {lead: 0.007, variants: 2, gain: 0.7, rank: 5},
  thud: {lead: 0.006, variants: 2, gain: 0.6, rank: 4},
  settle: {lead: 0.003, variants: 3, gain: 0.5, rank: 4},
  place: {lead: 0.01, variants: 3, gain: 0.55, rank: 4},
  swish: {lead: 0.12, variants: 3, gain: 0.35, rank: 3},
  tap: {lead: 0.005, variants: 3, gain: 0.3, rank: 2},
  tick: {lead: 0.002, variants: 5, gain: 0.22, rank: 1},
} as const;
export type Kind = keyof typeof KINDS;

export type Hit = {t: number; kind: Kind; beat: string; what: string};
export type Sound = Hit & {file: string; start: number; gain: number};

// two sounds closer than this keep only the higher rank; a tick yields only to what it would blur into
export const SPACING = 0.25;
export const TICK_SPACING = 0.08;
// a counter's ticks: fast at first, slowing to its stop, about the pace of a die rolling out
const TICK_FIRST = 0.07;
const TICK_LAST = 0.15;

// the ticks of a counter running from `a` to `b`, slowing down; the stop is its own sound
export const ticks = (a: number, b: number): number[] => {
  const out: number[] = [];
  for (let t = a; t < b - TICK_FIRST / 2; ) {
    out.push(t);
    t += TICK_FIRST + (TICK_LAST - TICK_FIRST) * ((t - a) / Math.max(b - a, 1e-6));
  }
  return out;
};

const PIN_TO_LINE = 72; // a one-line tag's text centre over its pin (carryMath TAG), near enough for a flight's length

const beatHits = (beat: Beat, prev: Beat | undefined, start: number, first: boolean): Hit[] => {
  const out: Hit[] = [];
  const hit = (t: number, kind: Kind, what: string) => out.push({t: start + t, kind, beat: beat.id, what});
  // coming in: a card is a page turned; a frame sliding or wiping in, cloth. A plain cut stays silent: the voice
  // carries it
  if (!first) {
    const across = beat.enter ? beat.enter.dur / 2 : 0;
    if (beat.kind === 'card') hit(across, 'page', 'card in');
    else if (beat.enter && (beat.enter.type === 'slide' || beat.enter.type === 'wipe')) hit(across, 'swish', `${beat.enter.type} in`);
  }
  if (beat.zoom && beat.zoom.dur <= 0.04) hit(beat.zoom.at, 'punch', 'punch-in');
  if (beat.carry && prev) {
    const c = beat.carry;
    const from = Array.isArray(c.from) ? c.from : [prev.tags[c.from.tag].x, prev.tags[c.from.tag].y - PIN_TO_LINE];
    const dur = c.dur ?? travelDur(Math.hypot(c.to[0] - from[0], c.to[1] - from[1]));
    hit(c.at + dur / 2, 'swish', 'carry');
    hit(c.at + dur, 'place', 'carry lands');
  }
  // a counter rolls and stops; the line after it (the verdict it causes) lands
  beat.tags.forEach((tag, i) => {
    tag.lines.forEach((line, j) => {
      const at = Math.max(tag.at, lineAt(tag, j));
      const stop = line.count ? lineAt(tag, j) + line.count.dur : 0;
      if (line.count && stop > at) { // Tags.tsx: a counter that stopped before its box opened shows only its number
        for (const t of ticks(at, stop)) hit(t, 'tick', `tag ${i} counter`);
        hit(stop, 'settle', `tag ${i} counter stops`);
      } else if (j > 0 && tag.lines[j - 1].count) {
        hit(at + MOTION.follow, 'place', `tag ${i} verdict`);
      }
    });
  });
  // a text says the point of a beat: it lands
  beat.texts.forEach((x, i) => hit(x.at + MOTION.follow, 'place', `text ${i}`));
  const c = beat.chart;
  if (!c) return out;
  if (c.type === 'formula') {
    c.terms.forEach((x, i) => hit(x.at, 'tap', `term ${i}`));
    if (c.example) {
      hit(c.example.at, 'tap', 'example');
      c.example.zones.forEach((z, i) => hit(z.at, i === 0 ? 'place' : 'tap', `result ${i}`));
    }
  } else if (c.type === 'bars') {
    c.groups.forEach((g, i) => hit(g.at, i === c.groups.length - 1 ? 'place' : 'tap', `group ${i}`));
  } else if (c.type === 'dots') {
    // a tick for each dot filling (the counter's number changes with it), the last one stops it
    for (let k = 1; k <= c.hits; k++) {
      const t = c.at + (c.dur * k) / (c.hits + 0.999);
      hit(t, k === c.hits ? 'settle' : 'tick', k === c.hits ? 'dots full' : 'dot');
    }
  } else if (c.type === 'stats') {
    hit(c.question.at, 'place', 'question');
  } else if (c.type === 'dice') {
    if (c.stepMark) hit(c.stepMark.at + MOTION.base, 'thud', 'step down');
    c.markers.forEach((m, i) => hit(m.at, 'place', `marker ${i}`));
  } else if (c.type === 'big') {
    c.items.forEach((x, i) => hit(x.at, 'place', `item ${i}`));
  }
  return out;
};

// every hit of the plan, before thinning, in seconds of the short
export const hits = (plan: Pick<Plan, 'beats'>): Hit[] => {
  let start = 0;
  const out: Hit[] = [];
  plan.beats.forEach((beat, i) => {
    out.push(...beatHits(beat, plan.beats[i - 1], start, i === 0));
    start += beat.dur;
  });
  return out.sort((a, b) => a.t - b.t);
};

// the hits that stay: the higher rank first; a sound keeps SPACING from every other kept one, a tick only
// TICK_SPACING (a counter's ticks come faster than that between themselves)
export const thin = (all: Hit[]): Hit[] => {
  const kept: Hit[] = [];
  const order = [...all].sort((a, b) => KINDS[b.kind].rank - KINDS[a.kind].rank || a.t - b.t);
  for (const h of order) {
    const near = kept.some((k) => {
      const gap = Math.abs(k.t - h.t);
      return h.kind === 'tick' || k.kind === 'tick' ? gap < TICK_SPACING && !(h.kind === 'tick' && k.kind === 'tick') : gap < SPACING;
    });
    if (!near && h.t >= 0) kept.push(h);
  }
  return kept.sort((a, b) => a.t - b.t);
};

// the sounds to play: a variant per hit (never the same one twice running for a kind), its file and start
export const sounds = (plan: Pick<Plan, 'beats'>): Sound[] => {
  const last: Partial<Record<Kind, number>> = {};
  return thin(hits(plan)).map((h, i) => {
    const k = KINDS[h.kind];
    const prev = last[h.kind];
    let n = (i * 7 + Math.round(h.t * 13)) % k.variants;
    if (n === prev) n = (n + 1) % k.variants;
    last[h.kind] = n;
    return {...h, file: `sfx/${h.kind}-${n}.wav`, start: h.t - k.lead, gain: k.gain};
  });
};
