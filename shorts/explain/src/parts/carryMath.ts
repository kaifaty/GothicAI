import {chance} from '../dice.ts';
import type {TagT} from '../schema';

export type Point = {x: number; y: number};
export type Size = {width: number; height: number};

// the tag box of Tags.tsx: its bottom 30 px over the pin, padding 12 × 22, lines 2 px apart, a line 52 px
export const TAG = {lift: 30, padY: 12, padX: 22, gap: 2, font: 52, edge: 36};

// `{chance:exe:40:14.7}`: the chance by that rule for that talent and distance, as the game counts it
export const withChances = (text: string) =>
  text.replace(/\{chance:(exe|opengothic):(\d+):([\d.]+)\}/g, (_, rule, talent, d) =>
    String(Math.round(chance(rule, Number(talent), Number(d)))),
  );

// a tag line as it reads once settled: its chances computed, its counter stopped on the real number
export const settledText = (line: TagT['lines'][number]) => {
  const text = withChances(line.text);
  return line.count ? text.replace('{n}', String(line.count.to)) : text;
};

// the centre of line `index` of a tag at time `t` of its beat, in frame pixels: where Tags.tsx draws it once every
// line shown by then has settled. `measure` gives a line's box (measureText in the render, a stub in the tests)
export const tagLineCentre = (tag: TagT, index: number, t: number, measure: (text: string) => Size): Point => {
  const lines = tag.lines.map((line, i) => ({i, size: measure(settledText(line)), at: line.at})).filter((l) => l.at < t);
  const width = Math.max(...lines.map((l) => l.size.width)) + 2 * TAG.padX;
  const height = lines.reduce((s, l) => s + l.size.height, 0) + TAG.gap * (lines.length - 1) + 2 * TAG.padY;
  let y = tag.y - TAG.lift - height + TAG.padY;
  let centre = 0;
  for (const l of lines) {
    if (l.i === index) centre = y + l.size.height / 2;
    y += l.size.height + TAG.gap;
  }
  const x = tag.side === 'center' ? tag.x : tag.side === 'right' ? tag.x - TAG.edge + width / 2 : tag.x + TAG.edge - width / 2;
  return {x, y: centre};
};

// the arc of a carry: a quadratic curve whose middle bows `arc` × the distance off the straight line, upward
// (or to the right for a straight vertical flight)
export const arcPoint = (a: Point, b: Point, arc: number, p: number): Point => {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  // the normal (−dy, dx) or (dy, −dx), whichever points up; for a vertical flight, the one pointing right
  let nx = -dy;
  let ny = dx;
  if (ny > 0 || (ny === 0 && nx < 0)) {
    nx = -nx;
    ny = -ny;
  }
  // the control point sits twice as far off the line as the curve's middle
  const c = {x: (a.x + b.x) / 2 + nx * arc * 2, y: (a.y + b.y) / 2 + ny * arc * 2};
  const q = 1 - p;
  return {x: q * q * a.x + 2 * q * p * c.x + p * p * b.x, y: q * q * a.y + 2 * q * p * c.y + p * p * b.y};
};

export const distance = (a: Point, b: Point) => Math.hypot(b.x - a.x, b.y - a.y);
