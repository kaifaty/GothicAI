import {Easing, interpolate} from 'remotion';

// how much of a thing shown from `at` to `until` (seconds) is visible at `t`: a short fade in, a shorter one out;
// fadeIn 0 is there from its first frame (a base layer on a cut: frame 0 is a finished picture)
export const shown = (t: number, at: number, until?: number, fadeIn = 0.18, fadeOut = 0.12) => {
  const a = fadeIn <= 0 ? Number(t >= at)
    : interpolate(t, [at, at + fadeIn], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
  if (until === undefined) {
    return a;
  }
  const b = interpolate(t, [until - fadeOut, until], [1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'});
  return Math.min(a, b);
};

// a rise of a few pixels while it fades in
export const rise = (t: number, at: number, px = 18) =>
  interpolate(t, [at, at + 0.25], [px, 0], {
    extrapolateLeft: 'clamp',
    extrapolateRight: 'clamp',
    easing: Easing.out(Easing.cubic),
  });

// the value of [[t, v], ...] keyframes at t, eased between them
export const keyframes = (t: number, keys: [number, number][]) => {
  if (keys.length === 1) {
    return keys[0][1];
  }
  return interpolate(
    t,
    keys.map((k) => k[0]),
    keys.map((k) => k[1]),
    {extrapolateLeft: 'clamp', extrapolateRight: 'clamp', easing: Easing.inOut(Easing.quad)},
  );
};
