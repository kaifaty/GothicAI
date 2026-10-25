export type Point = {x: number; y: number};
export type Viewport = {left: number; top: number; width: number; height: number};
export type Zoom = {at: number; dur: number; scale: number; x: number; y: number; ease?: 'inOut' | 'linear'};

// the camera's curve: it starts and stops softly (cubic in-out); linear only for a slow push across a whole beat
const easeInOut = (p: number) => (p < 0.5 ? 4 * p * p * p : 1 - Math.pow(-2 * p + 2, 3) / 2);
export type Action = {frame: number; action: string; id: number; t: number};
export type TrackSample = readonly [frame: number, x: number | null, y: number | null];
export type HpSample = readonly [frame: number, self: number, target: number];

export const zoomScale = (zoom: Zoom | undefined, t: number): number => {
  if (!zoom) return 1;
  const progress = zoom.dur <= 0 ? Number(t >= zoom.at) : Math.max(0, Math.min(1, (t - zoom.at) / zoom.dur));
  return 1 + (zoom.scale - 1) * (zoom.ease === 'linear' ? progress : easeInOut(progress));
};

// Match Picture's centered object-fit: cover, then CSS scale around its local origin.
export const transformPoint = (
  point: Point,
  sourceSize: readonly [number, number],
  viewport: Viewport,
  zoom?: Zoom,
  t = 0,
): Point => {
  const [sourceWidth, sourceHeight] = sourceSize;
  const cover = Math.max(viewport.width / sourceWidth, viewport.height / sourceHeight);
  const x = point.x * cover + (viewport.width - sourceWidth * cover) / 2;
  const y = point.y * cover + (viewport.height - sourceHeight * cover) / 2;
  const scale = zoomScale(zoom, t);
  const originX = zoom?.x ?? viewport.width / 2;
  const originY = zoom?.y ?? viewport.height / 2;
  return {
    x: viewport.left + originX + (x - originX) * scale,
    y: viewport.top + originY + (y - originY) * scale,
  };
};

// OffthreadVideo rounds startFrom separately from the beat's current/frozen frame.
export const sourceFrame = (from: number, t: number, fps: number, freeze?: number): number =>
  Math.round(from * fps) + Math.round(Math.min(t, freeze ?? Infinity) * fps);

// Data is ordered by source frame. Return the latest sample without assuming dense frames.
const latestIndex = (samples: readonly (readonly [number, ...unknown[]])[], frame: number): number => {
  let low = 0;
  let high = samples.length;
  while (low < high) {
    const middle = Math.floor((low + high) / 2);
    if (samples[middle][0] <= frame) low = middle + 1;
    else high = middle;
  }
  return low - 1;
};

export const selectAction = <T extends Action>(
  actions: readonly T[],
  names: Readonly<Record<string, string>>,
  frame: number,
  fps: number,
  hold = 1.2,
): (T & {name: string}) | null => {
  let low = 0;
  let high = actions.length;
  while (low < high) {
    const middle = Math.floor((low + high) / 2);
    if (actions[middle].frame <= frame) low = middle + 1;
    else high = middle;
  }
  const action = actions[low - 1];
  if (!action || frame >= action.frame + hold * fps) return null;
  const name = Object.hasOwn(names, action.action) ? names[action.action] : undefined;
  return name ? {...action, name} : null;
};

// A centered triangular filter is reproducible when Remotion renders frames out of order.
// Null samples cut the filter window and hide the label on their very first frame.
export const smoothedTrack = (track: readonly TrackSample[], frame: number, fps: number): Point | null => {
  const index = latestIndex(track, frame);
  const sample = track[index];
  if (!sample || sample[1] === null || sample[2] === null) return null;
  const radius = Math.max(1, Math.round(fps * 0.1));
  let sumX = 0;
  let sumY = 0;
  let total = 0;
  const add = (row: TrackSample) => {
    const distance = Math.abs(row[0] - frame);
    if (distance > radius || row[1] === null || row[2] === null) return;
    const weight = radius + 1 - distance;
    sumX += row[1] * weight;
    sumY += row[2] * weight;
    total += weight;
  };
  add(sample);
  for (let i = index - 1; i >= 0; i--) {
    const row = track[i];
    if (row[0] < frame - radius || row[1] === null || row[2] === null) break;
    add(row);
  }
  for (let i = index + 1; i < track.length; i++) {
    const row = track[i];
    if (row[0] > frame + radius || row[1] === null || row[2] === null) break;
    add(row);
  }
  return total ? {x: sumX / total, y: sumY / total} : {x: sample[1], y: sample[2]};
};

export const sampleHp = (hp: readonly HpSample[], frame: number): [number, number] | null => {
  const sample = hp[latestIndex(hp, frame)];
  return sample ? [sample[1], sample[2]] : null;
};
