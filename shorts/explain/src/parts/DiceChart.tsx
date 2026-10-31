import React from 'react';
import {AbsoluteFill, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';
import {chance, exeChance, MAX_DIST, MIN_DIST, type Curve} from '../dice';
import type {DiceChart as Spec} from '../schema';
import {COLOR, FONT, OUTLINE} from '../theme';
import {keyframes, shown} from './anim';

const STYLE: Record<Curve, {color: string; width: number}> = {
  exe: {color: COLOR.now, width: 9},
  opengothic: {color: COLOR.before, width: 6},
};

// the line's corners up to `upTo` metres (both rules are straight pieces); the exe's step at 15 m drops as `drop`
// goes 0 -> 1. Drawn without the game's rounding to whole percent; the markers show the game's numbers.
const points = (curve: Curve, talent: number, upTo: number, drop: number, x: (d: number) => number, y: (c: number) => number) => {
  const raw = (d: number) =>
    curve === 'exe' && d > MIN_DIST ? talent - (talent * d) / MAX_DIST : chance(curve, talent, d);
  const pts: [number, number][] = [[x(0), y(raw(0))]];
  if (upTo <= 0) {
    return pts;
  }
  pts.push([x(Math.min(upTo, MIN_DIST)), y(raw(Math.min(upTo, MIN_DIST)))]);
  if (curve === 'exe' && upTo >= MIN_DIST) {
    pts.push([x(MIN_DIST), y(talent - (drop * talent) / 3)]);
  }
  if (upTo > MIN_DIST && (curve !== 'exe' || drop >= 1)) {
    pts.push([x(upTo), y(raw(upTo))]);
  }
  return pts;
};

const label = (size: number, color: string): React.CSSProperties => ({
  fontFamily: FONT.body,
  fontWeight: 800,
  fontSize: size,
  color,
  textShadow: OUTLINE,
  fontVariantNumeric: 'tabular-nums',
  whiteSpace: 'nowrap',
});

export const DiceChart: React.FC<{spec: Spec}> = ({spec}) => {
  const {width, height, fps} = useVideoConfig();
  const t = useCurrentFrame() / fps;
  const wide = width > height;
  // Portrait coordinates stay fixed; the landscape plot uses the available width above the captions.
  const X0 = wide ? 230 : 190;
  const X1 = wide ? 1740 : 900;
  const Y0 = wide ? 740 : 1040;
  const Y1 = wide ? 260 : 470;
  const x = (d: number) => X0 + ((X1 - X0) * d) / MAX_DIST;
  const y = (c: number) => Y0 + ((Y1 - Y0) * c) / 100;
  const axes = shown(t, 0, undefined, 0.3);
  const H = spec.talent;
  const drop = spec.stepMark
    ? interpolate(t, [spec.stepMark.at, spec.stepMark.at + 0.35], [0, 1], {
        extrapolateLeft: 'clamp',
        extrapolateRight: 'clamp',
      })
    : 1;

  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <svg width={width} height={height} style={{position: 'absolute', opacity: axes}}>
        {[0, 25, 50, 75, 100].map((c) => (
          <line key={c} x1={X0} x2={X1} y1={y(c)} y2={y(c)} stroke={COLOR.grid} strokeWidth={2} />
        ))}
        {[0, 15, 30, 45].map((d) => (
          <line key={d} x1={x(d)} x2={x(d)} y1={Y0} y2={Y1} stroke={COLOR.grid} strokeWidth={2} />
        ))}
        {spec.talentMark && (
          <line
            x1={X0}
            x2={x(MIN_DIST)}
            y1={y(H)}
            y2={y(H)}
            stroke={COLOR.skill}
            strokeWidth={4}
            strokeDasharray="14 10"
            opacity={shown(t, spec.talentMark.at)}
          />
        )}
        {spec.curves.map((c) => {
          const upTo = keyframes(t, c.draw);
          const pts = points(c.curve, H, upTo, drop, x, y);
          if (pts.length < 2) {
            return null;
          }
          return (
            <polyline
              key={c.curve}
              points={pts.map((p) => p.join(',')).join(' ')}
              fill="none"
              stroke={STYLE[c.curve].color}
              strokeWidth={STYLE[c.curve].width}
              strokeLinejoin="round"
              strokeLinecap="round"
            />
          );
        })}
        {spec.markers.map((m, i) => {
          const o = shown(t, m.at);
          const v = m.curve === 'exe' ? exeChance(H, m.d) : chance(m.curve, H, m.d);
          return o > 0 ? (
            <circle key={i} cx={x(m.d)} cy={y(v)} r={13} fill={STYLE[m.curve].color} stroke="#000" strokeWidth={3} opacity={o} />
          ) : null;
        })}
      </svg>

      <div style={{position: 'absolute', left: X0 - 20, top: Y1 - 90, opacity: axes, ...label(40, COLOR.text)}}>
        {spec.axisY}
      </div>
      {[0, 50, 100].map((c) => (
        <div
          key={c}
          style={{position: 'absolute', right: width - X0 + 18, top: y(c) - 24, opacity: axes, ...label(36, COLOR.before)}}
        >
          {c}
        </div>
      ))}
      {[0, 15, 30, 45].map((d) => (
        <div
          key={d}
          style={{
            position: 'absolute',
            left: x(d),
            top: Y0 + 16,
            transform: 'translateX(-50%)',
            opacity: axes,
            ...label(36, d === MIN_DIST ? COLOR.text : COLOR.before),
          }}
        >
          {d}
        </div>
      ))}
      <div
        style={{position: 'absolute', left: X1, top: Y0 + 64, transform: 'translateX(-100%)', opacity: axes, ...label(40, COLOR.text)}}
      >
        {spec.axisX}
      </div>

      {spec.talentMark && (
        <div
          style={{
            position: 'absolute',
            left: x(MIN_DIST) - 22,
            top: y(H) + 10,
            transform: 'translateX(-100%)',
            opacity: shown(t, spec.talentMark.at),
            ...label(40, COLOR.skill),
          }}
        >
          {spec.talentMark.text}
        </div>
      )}
      {spec.stepMark && (
        <div
          style={{
            position: 'absolute',
            left: x(MIN_DIST) + 30,
            top: y(H) - (wide ? 94 : 112),
            opacity: shown(t, spec.stepMark.at + 0.2),
            ...label(wide ? 72 : 88, COLOR.text),
          }}
        >
          {spec.stepMark.text}
        </div>
      )}
      {spec.markers.map((m, i) => {
        const o = shown(t, m.at);
        if (o <= 0) {
          return null;
        }
        const exact = m.curve === 'exe' ? exeChance(H, m.d) : chance(m.curve, H, m.d);
        const exe = m.curve === 'exe';
        return (
          <div
            key={i}
            style={{
              position: 'absolute',
              left: wide ? Math.max(120, Math.min(1510, x(m.d) + (exe ? -240 : 26))) : exe ? x(m.d) - 30 : x(m.d) + 26,
              top: y(exact) + (exe ? 26 : -84),
              transform: !wide && exe ? 'translateX(-100%)' : undefined,
              opacity: o,
              ...label(48, STYLE[m.curve].color),
              ...(wide ? {width: 310, whiteSpace: 'normal', lineHeight: 1.15} : {}),
            }}
          >
            {m.text.replace('{v}', String(Math.round(exact)))}
          </div>
        );
      })}
      {spec.curves.map((c, i) => (
        <div
          key={c.curve}
          style={{
            position: 'absolute',
            left: X1,
            top: Y1 - 150 + 52 * i,
            transform: 'translateX(-100%)',
            display: 'flex',
            alignItems: 'center',
            gap: 14,
            opacity: shown(t, c.draw[0][0]),
            ...label(38, STYLE[c.curve].color),
          }}
        >
          <span style={{width: 44, height: STYLE[c.curve].width, background: STYLE[c.curve].color, borderRadius: 4}} />
          {c.name}
        </div>
      ))}

      <div style={{position: 'absolute', left: wide ? 96 : X0 - 20, top: wide ? 850 : 1440, opacity: axes, ...label(28, COLOR.before), fontWeight: 600, ...(wide ? {width: 1728, whiteSpace: 'normal'} : {})}}>
        {spec.source}
      </div>
    </AbsoluteFill>
  );
};
