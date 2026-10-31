// The series' durations in seconds, without Remotion, so the plan's schema and the tests share them with motion.ts.

export const MOTION = {
  fast: 0.2, // a small thing appearing: a label line, a number
  base: 0.35, // a text, a panel, a fade or a flash between beats
  frame: 0.5, // the whole frame travelling: a slide or wipe between beats (twice the travel of a panel, ~40% longer)
  slow: 0.6, // a big move: a zoom, a line drawing itself
  stagger: 0.08, // between items of one group entering together
  follow: 0.06, // an attached part settling after its main body: a tag's box after its pin, words after their band
};

// a beat's transition when the plan gives no `dur`: what travels across the frame takes longer than what fades
export const TRANSITION_DUR = {
  fade: MOTION.base,
  flash: MOTION.base,
  zoom: MOTION.base,
  slide: MOTION.frame,
  wipe: MOTION.frame,
} as const;

// a thing flying across the frame (a carry): longer the farther it goes, about +40% for twice the distance, from
// `base` for a short hop (300 px) up to `slow` across the frame
export const TRAVEL_PX = 300;
export const travelDur = (px: number) => Math.min(MOTION.slow, Math.max(MOTION.base, MOTION.base * Math.sqrt(px / TRAVEL_PX)));

// a slow push on a held picture (a card, a still, footage after its freeze) of HOLD_PUSH s or more, so a long hold
// does not read as a frozen screen: linear across the hold, 1 % of scale per second held, between 3 and 5 %, so a
// longer hold pushes slower, not further. A plan's `zoom` replaces it, `push: 0` turns it off, a number sets the scale
export const HOLD_PUSH = 2;
export const PUSH_RATE = 0.01;
export const PUSH_MIN = 0.03;
export const PUSH_MAX = 0.05;

type Held = {kind: string; dur: number; freeze?: number; zoom?: unknown; push?: number};

export const holdPush = (beat: Held): {at: number; dur: number; scale: number} | null => {
  if (beat.zoom || beat.push === 0 || beat.kind === 'split') return null;
  const at = beat.kind === 'footage' ? beat.freeze : 0;
  if (at === undefined) return null;
  const dur = beat.dur - at;
  if (dur < HOLD_PUSH) return null;
  return {at, dur, scale: 1 + (beat.push ?? Math.min(PUSH_MAX, Math.max(PUSH_MIN, PUSH_RATE * dur)))};
};
