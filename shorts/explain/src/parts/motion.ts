import type React from 'react';
import {Easing, interpolate, spring} from 'remotion';
import {MOTION} from './timing';

// The series' motion: one set of durations (timing.ts), curves and springs, so every short moves alike. A plan
// picks from these by name; a new movement is added here, not tuned per short.

export {MOTION, TRANSITION_DUR} from './timing';

// out: arrives fast and settles (entrances); in: leaves slowly then fast (exits); inOut: moves between two rests
export const EASE = {
  out: Easing.bezier(0.22, 1, 0.36, 1),
  in: Easing.bezier(0.64, 0, 0.78, 0),
  inOut: Easing.inOut(Easing.cubic),
};

export const SPRING = {
  pop: {damping: 12, stiffness: 180, mass: 0.6}, // a small overshoot: labels, numbers, badges
  settle: {damping: 200}, // no overshoot: panels, pictures
};

// 0 -> 1 from `at` (seconds of the beat) along a spring; a pop overshoots a little above 1 before it rests
export const springAt = (frame: number, fps: number, at: number, config = SPRING.pop) =>
  spring({frame: frame - Math.round(at * fps), fps, config});

// 0 -> 1 from `at` over `dur` along a curve, clamped
export const progress = (t: number, at: number, dur: number, easing = EASE.out) =>
  dur <= 0
    ? Number(t >= at)
    : interpolate(t, [at, at + dur], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp', easing});

export type Enter = 'rise' | 'pop' | 'slide';

// how a text or a label comes in at `at`: rise (a few pixels up), pop (a spring scale) or slide (from the left)
export const enterTransform = (kind: Enter, t: number, frame: number, fps: number, at: number): string => {
  if (kind === 'pop') {
    return `scale(${0.82 + 0.18 * springAt(frame, fps, at)})`;
  }
  if (kind === 'slide') {
    return `translateX(${-60 * (1 - progress(t, at, MOTION.base))}px)`;
  }
  return `translateY(${18 * (1 - progress(t, at, 0.25))}px)`;
};

// a transition into a beat. The beat starts at its own time; the one before it stays under it for `dur`, so the
// voice, the captions and the beats' times do not move. flash is a hard cut with a white flash on the new beat.
export type TransitionType = 'fade' | 'slide' | 'wipe' | 'zoom' | 'flash';

export const enterStyle = (type: TransitionType, p: number, wide: boolean): React.CSSProperties => {
  if (type === 'fade') return {opacity: p};
  if (type === 'slide') {
    const k = (1 - EASE.out(p)) * 100;
    return {transform: wide ? `translateX(${k}%)` : `translateY(${k}%)`};
  }
  if (type === 'wipe') {
    const k = (1 - EASE.inOut(p)) * 100;
    return {clipPath: wide ? `inset(0 ${k}% 0 0)` : `inset(${k}% 0 0 0)`};
  }
  if (type === 'zoom') {
    const k = EASE.out(p);
    return {opacity: Math.min(1, p * 2), transform: `scale(${1.12 - 0.12 * k})`};
  }
  return {};
};

// what the beat underneath does while the next one comes in
export const exitStyle = (type: TransitionType, p: number, wide: boolean): React.CSSProperties => {
  if (type === 'slide') {
    const k = -EASE.out(p) * 30;
    return {transform: wide ? `translateX(${k}%)` : `translateY(${k}%)`, filter: `brightness(${1 - 0.4 * p})`};
  }
  if (type === 'zoom') return {transform: `scale(${1 + 0.06 * EASE.in(p)})`};
  if (type === 'wipe') return {filter: `brightness(${1 - 0.3 * p})`};
  return {};
};

// the white of a flash cut over the first frames of the new beat
export const flashOpacity = (p: number) => 0.85 * (1 - EASE.out(p));
