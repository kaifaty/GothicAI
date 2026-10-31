import React from 'react';
import {AbsoluteFill, Easing, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';
import {evolvePath, getLength, getPointAtLength, getTangentAtLength} from '@remotion/paths';
import type {FanT, PathT, StrokeT} from '../schema';
import {shown} from './anim';
import {EASE, progress} from './motion';

const grow = (t: number, at: number, dur: number) =>
  interpolate(t, [at, at + dur], [0, 1], {
    extrapolateLeft: 'clamp',
    extrapolateRight: 'clamp',
    easing: Easing.inOut(Easing.cubic),
  });

const lerp = (a: [number, number], b: [number, number], k: number): [number, number] => [
  a[0] + (b[0] - a[0]) * k,
  a[1] + (b[1] - a[1]) * k,
];

const dist = (a: [number, number], b: [number, number]) => Math.hypot(b[0] - a[0], b[1] - a[1]);

// a segment with a dark edge under it, so it reads over a bright and a dark picture alike
const Seg: React.FC<{a: [number, number]; b: [number, number]; color: string; width: number; dash?: string}> = ({
  a,
  b,
  color,
  width,
  dash,
}) => (
  <>
    <line x1={a[0]} y1={a[1]} x2={b[0]} y2={b[1]} stroke="rgba(0,0,0,0.6)" strokeWidth={width + 6} strokeLinecap="round" strokeDasharray={dash} />
    <line x1={a[0]} y1={a[1]} x2={b[0]} y2={b[1]} stroke={color} strokeWidth={width} strokeLinecap="round" strokeDasharray={dash} />
  </>
);

const Line: React.FC<{s: StrokeT; t: number}> = ({s, t}) => {
  const o = shown(t, s.at, s.until);
  if (o <= 0) {
    return null;
  }
  const k = grow(t, s.at, s.dur);
  const total = dist(s.from, s.to);
  const end = lerp(s.from, s.to, k);
  const cutShown = s.cut !== undefined && s.cutAt !== undefined && t >= s.cutAt;
  const cutK = s.cut ? dist(s.from, s.cut) / Math.max(total, 1) : 1;
  const parts: React.ReactNode[] = [];
  if (cutShown && s.cut && k > cutK) {
    const past = shown(t, s.cutAt!, undefined, 0.25);
    const cc = s.cutColor ?? s.color;
    parts.push(<Seg key="a" a={s.from} b={s.cut} color={cc} width={s.width} />);
    parts.push(
      <g key="b" opacity={0.55 * past}>
        <Seg a={s.cut} b={end} color={cc} width={s.width * 0.6} dash={`${s.width * 1.6} ${s.width * 1.8}`} />
      </g>,
    );
    // a cross where it stops
    const r = s.width * 2.6;
    const [x, y] = s.cut;
    parts.push(
      <g key="x" opacity={past}>
        <Seg a={[x - r, y - r]} b={[x + r, y + r]} color={cc} width={s.width * 0.9} />
        <Seg a={[x - r, y + r]} b={[x + r, y - r]} color={cc} width={s.width * 0.9} />
      </g>,
    );
  } else {
    parts.push(<Seg key="a" a={s.from} b={end} color={s.color} width={s.width} />);
  }
  return <g opacity={o}>{parts}</g>;
};

const FanView: React.FC<{f: FanT; t: number}> = ({f, t}) => {
  const o = shown(t, f.at, f.until);
  if (o <= 0) {
    return null;
  }
  // as long as the edge toward `b` (the one toward `a` may run far off the picture)
  const [cx, cy] = f.apex;
  const r = dist(f.apex, f.b);
  const a0 = Math.atan2(f.a[1] - cy, f.a[0] - cx);
  let a1 = Math.atan2(f.b[1] - cy, f.b[0] - cx);
  // the short way round
  while (a1 - a0 > Math.PI) a1 -= 2 * Math.PI;
  while (a1 - a0 < -Math.PI) a1 += 2 * Math.PI;
  const k = grow(t, f.at, f.dur);
  const am = a0 + (a1 - a0) * k;
  const pa: [number, number] = [cx + r * Math.cos(a0), cy + r * Math.sin(a0)];
  const pb: [number, number] = [cx + r * Math.cos(am), cy + r * Math.sin(am)];
  const sweep = am > a0 ? 1 : 0;
  const large = Math.abs(am - a0) > Math.PI ? 1 : 0;
  const d = `M ${cx} ${cy} L ${pa[0]} ${pa[1]} A ${r} ${r} 0 ${large} ${sweep} ${pb[0]} ${pb[1]} Z`;
  return (
    <g opacity={o}>
      <path d={d} fill={f.color} fillOpacity={0.22} />
      <Seg a={f.apex} b={pa} color={f.color} width={5} />
      <Seg a={f.apex} b={pb} color={f.color} width={5} />
    </g>
  );
};

// an SVG path through the points: straight segments, or a Catmull-Rom curve through every point
export const pathThrough = (points: [number, number][], curve: boolean): string => {
  const [x0, y0] = points[0];
  if (!curve || points.length === 2) {
    return `M ${x0} ${y0} ` + points.slice(1).map(([x, y]) => `L ${x} ${y}`).join(' ');
  }
  let d = `M ${x0} ${y0}`;
  for (let i = 0; i < points.length - 1; i++) {
    const p0 = points[Math.max(0, i - 1)];
    const p1 = points[i];
    const p2 = points[i + 1];
    const p3 = points[Math.min(points.length - 1, i + 2)];
    const c1 = [p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6];
    const c2 = [p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6];
    d += ` C ${c1[0]} ${c1[1]} ${c2[0]} ${c2[1]} ${p2[0]} ${p2[1]}`;
  }
  return d;
};

// a path drawing itself: the stroke grows along it (in-out, like the camera), an arrowhead rides the tip
const PathView: React.FC<{p: PathT; t: number; id: string}> = ({p, t, id}) => {
  const o = shown(t, p.at, p.until);
  if (o <= 0) {
    return null;
  }
  const d = pathThrough(p.points, p.curve);
  const k = progress(t, p.at, p.dur, EASE.inOut);
  const evolved = evolvePath(k, d);
  const under = {d, fill: 'none', strokeLinecap: 'round' as const, strokeLinejoin: 'round' as const};
  const dash = p.dash ? `${p.width * 1.6} ${p.width * 1.8}` : undefined;
  const len = getLength(d) * k;
  const tip = getPointAtLength(d, Math.max(len, 0.01)) ?? {x: p.points[0][0], y: p.points[0][1]};
  const dir = getTangentAtLength(d, Math.max(len, 0.01)) ?? {x: 1, y: 0};
  const a = Math.atan2(dir.y, dir.x);
  const r = p.width * 2.4;
  const head = [
    [tip.x + Math.cos(a) * r, tip.y + Math.sin(a) * r],
    [tip.x + Math.cos(a + 2.4) * r, tip.y + Math.sin(a + 2.4) * r],
    [tip.x + Math.cos(a - 2.4) * r, tip.y + Math.sin(a - 2.4) * r],
  ].map((q) => q.join(',')).join(' ');
  return (
    <g opacity={o}>
      {p.dash && (
        <mask id={id} maskUnits="userSpaceOnUse">
          <path {...under} stroke="#fff" strokeWidth={p.width + 8} {...evolved} />
        </mask>
      )}
      <g mask={p.dash ? `url(#${id})` : undefined}>
        <path {...under} stroke="rgba(0,0,0,0.6)" strokeWidth={p.width + 6} {...(p.dash ? {strokeDasharray: dash} : evolved)} />
        <path {...under} stroke={p.color} strokeWidth={p.width} {...(p.dash ? {strokeDasharray: dash} : evolved)} />
      </g>
      {p.arrow && k > 0 && <polygon points={head} fill={p.color} stroke="rgba(0,0,0,0.6)" strokeWidth={3} />}
    </g>
  );
};

// lines of sight, drawn paths and the focus fan over the picture, from the bench's exports
export const Sight: React.FC<{strokes: StrokeT[]; fans: FanT[]; paths?: PathT[]; scope?: string}> = ({strokes, fans, paths = [], scope = ''}) => {
  const {fps, width, height} = useVideoConfig();
  const t = useCurrentFrame() / fps;
  if (strokes.length === 0 && fans.length === 0 && paths.length === 0) {
    return null;
  }
  return (
    <AbsoluteFill>
      <svg width={width} height={height} viewBox={`0 0 ${width} ${height}`}>
        {fans.map((f, i) => (
          <FanView key={`f${i}`} f={f} t={t} />
        ))}
        {strokes.map((s, i) => (
          <Line key={`s${i}`} s={s} t={t} />
        ))}
        {paths.map((p, i) => (
          <PathView key={`p${i}`} p={p} t={t} id={`path-mask-${scope}-${i}`} />
        ))}
      </svg>
    </AbsoluteFill>
  );
};
