// Diagram coordinates use centimetres: X points right, Y points forward (up on screen).
export const clamp01 = (v: number) => Math.max(0, Math.min(1, v));

export type PathPoint = {t: number; x: number; y: number};

export const interpolatePath = (path: readonly PathPoint[], t: number): {x: number; y: number} => {
  const first = path[0];
  if (t <= first.t) return {x: first.x, y: first.y};
  for (let i = 1; i < path.length; i++) {
    const a = path[i - 1];
    const b = path[i];
    if (t <= b.t) {
      const k = (t - a.t) / (b.t - a.t);
      return {x: a.x + (b.x - a.x) * k, y: a.y + (b.y - a.y) * k};
    }
  }
  const last = path[path.length - 1];
  return {x: last.x, y: last.y};
};

export const inCone = (point: {x: number; y: number}, halfDeg: number, rangeCm: number, turnDeg = 0) => {
  const bearing = Math.atan2(point.x, point.y) * 180 / Math.PI;
  const angle = ((bearing - turnDeg + 180) % 360 + 360) % 360 - 180;
  return Math.hypot(point.x, point.y) <= rangeCm && Math.abs(angle) <= halfDeg;
};

type XY = {x: number; y: number};
export const quadraticPoint = (a: XY, c: XY, b: XY, k: number): XY => ({
  x: (1 - k) ** 2 * a.x + 2 * (1 - k) * k * c.x + k ** 2 * b.x,
  y: (1 - k) ** 2 * a.y + 2 * (1 - k) * k * c.y + k ** 2 * b.y,
});

// SVG dashoffset advances by arc length, not by the Bezier parameter.
export const quadraticParameterAtLength = (a: XY, c: XY, b: XY, progress: number): number => {
  if (progress <= 0) return 0;
  if (progress >= 1) return 1;
  const samples = 256;
  const lengths = [0];
  let previous = a;
  for (let i = 1; i <= samples; i++) {
    const point = quadraticPoint(a, c, b, i / samples);
    lengths.push(lengths[i - 1] + Math.hypot(point.x - previous.x, point.y - previous.y));
    previous = point;
  }
  const total = lengths[samples];
  if (total === 0) return 0;
  const distance = progress * total;
  let low = 0, high = samples;
  while (high - low > 1) {
    const middle = Math.floor((low + high) / 2);
    if (lengths[middle] < distance) low = middle;
    else high = middle;
  }
  return (low + (distance - lengths[low]) / (lengths[high] - lengths[low])) / samples;
};
