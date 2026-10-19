import React from 'react';
import {AbsoluteFill, Easing, interpolate, useCurrentFrame, useVideoConfig} from 'remotion';
import type {BarsCard as Bars, DotsCard as Dots, FormulaCard as Formula, StatsCard as Stats} from '../schema';
import {COLOR, FONT, OUTLINE, SAFE, W} from '../theme';
import {rise, shown} from './anim';

// cards: what has no picture in the world. Numbers are computed here from the plan's inputs, not typed in.

const LEFT = SAFE.left + 30;
const WIDTH = W - SAFE.left - SAFE.right - 60;

const text = (size: number, color: string = COLOR.text, weight = 800): React.CSSProperties => ({
  fontFamily: FONT.body,
  fontWeight: weight,
  fontSize: size,
  color,
  textShadow: OUTLINE,
  fontVariantNumeric: 'tabular-nums',
});

const Source: React.FC<{text: string}> = ({text: s}) => {
  const {width, height} = useVideoConfig();
  const wide = width > height;
  return <div style={{position: 'absolute', left: wide ? 96 : LEFT, top: wide ? 850 : 1440, width: wide ? 1728 : WIDTH, ...text(28, COLOR.before, 600)}}>{s}</div>;
};

const useT = () => useCurrentFrame() / useVideoConfig().fps;

const grow = (t: number, at: number, dur = 0.6) =>
  interpolate(t, [at, at + dur], [0, 1], {
    extrapolateLeft: 'clamp',
    extrapolateRight: 'clamp',
    easing: Easing.out(Easing.cubic),
  });

// the damage formula: terms appear as they are said and stay; the worked example is computed from its inputs
// the same way the engine does it (damagecalculator.cpp: rounded at each step, protection not below 0,
// at least 5 at the end)
export const FormulaView: React.FC<{spec: Formula}> = ({spec}) => {
  const t = useT();
  const ex = spec.example;
  const {width, height} = useVideoConfig();
  const wide = width > height;
  const num = (v: number, digits = 0) => v.toFixed(digits).replace('.', spec.decimal);
  const base = ex ? Math.round((ex.bow + ex.dex) * ex.draw) : 0;
  const rest = ex ? Math.max(0, base - ex.prot) : 0;
  // Landscape places the worked example alongside the terms; portrait keeps the original single column.
  if (wide) {
    return (
      <AbsoluteFill style={{background: COLOR.card}}>
        <div style={{position: 'absolute', left: 96, top: 145, width: 1728, opacity: shown(t, 0), ...text(76), fontFamily: FONT.title}}>{spec.title}</div>
        <div style={{position: 'absolute', left: 96, top: 260, width: 1728, height: 550, display: 'grid', gridTemplateColumns: ex ? '1fr 1fr' : '1fr', gap: 72}}>
          <div style={{minWidth: 0}}>
            {spec.terms.map((term, i) => (
              <div key={i} style={{opacity: shown(t, term.at), transform: `translateY(${rise(t, term.at, 14)}px)`, ...text(54, term.color ?? COLOR.text), lineHeight: 1.22}}>{term.text}</div>
            ))}
            {spec.legend && <div style={{marginTop: 22, opacity: shown(t, spec.legend.at), ...text(32, COLOR.before, 600), lineHeight: 1.35}}>{spec.legend.text}</div>}
          </div>
          {ex && (
            <div style={{minWidth: 0, opacity: shown(t, ex.at)}}>
              <div style={{...text(48), lineHeight: 1.3}}>({ex.bow} + {ex.dex}) × {num(ex.draw, 1)} − {ex.prot} = {rest}</div>
              {ex.zones.map((z, i) => (
                <div key={i} style={{marginTop: 14, opacity: shown(t, z.at), ...text(48, i === 0 ? COLOR.now : COLOR.text), lineHeight: 1.3}}>× {num(z.k, z.k % 1 ? 1 : 0)} {z.name} = {Math.max(5, Math.round(rest * z.k))}</div>
              ))}
            </div>
          )}
        </div>
        <Source text={spec.source} />
      </AbsoluteFill>
    );
  }
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <div style={{position: 'absolute', left: LEFT, top: 280, width: WIDTH, display: 'flex', flexDirection: 'column'}}>
        <div style={{opacity: shown(t, 0), ...text(84), fontFamily: FONT.title, marginBottom: 6}}>{spec.title}</div>
        {spec.terms.map((term, i) => (
          <div
            key={i}
            style={{
              opacity: shown(t, term.at),
              transform: `translateY(${rise(t, term.at, 14)}px)`,
              ...text(58, term.color ?? COLOR.text),
              lineHeight: 1.22,
            }}
          >
            {term.text}
          </div>
        ))}
        {spec.legend && (
          <div style={{marginTop: 14, opacity: shown(t, spec.legend.at), ...text(30, COLOR.before, 600), lineHeight: 1.35}}>
            {spec.legend.text}
          </div>
        )}
        {ex && (
          <div style={{marginTop: 22, opacity: shown(t, ex.at)}}>
            <div style={text(46)}>
              ({ex.bow} + {ex.dex}) × {num(ex.draw, 1)} − {ex.prot} = {rest}
            </div>
            {ex.zones.map((z, i) => (
              <div key={i} style={{marginTop: 6, opacity: shown(t, z.at), ...text(46, i === 0 ? COLOR.now : COLOR.text)}}>
                × {num(z.k, z.k % 1 ? 1 : 0)} {z.name} = {Math.max(5, Math.round(rest * z.k))}
              </div>
            ))}
          </div>
        )}
      </div>
      <Source text={spec.source} />
    </AbsoluteFill>
  );
};

// bars by groups, growing as each group is named; the value written over each bar
export const BarsView: React.FC<{spec: Bars}> = ({spec}) => {
  const t = useT();
  const {width, height} = useVideoConfig();
  const wide = width > height;
  const left = wide ? 96 : LEFT;
  const contentWidth = wide ? 1728 : WIDTH;
  const top = wide ? 360 : 560;
  const hasNotes = spec.groups.some((g) => g.note !== undefined);
  const bottom = wide ? (hasNotes ? 690 : 735) : 1060;
  const groupW = contentWidth / spec.groups.length;
  const barW = Math.min(wide ? 140 : 110, (groupW - 60) / spec.series.length);
  const values = spec.groups.flatMap((g) => g.values);
  const minPositive = Math.min(...values.filter((v) => v > 0));
  const logBase = Number.isFinite(minPositive)
    ? Math.min(1, 10 ** (Math.ceil(Math.log10(minPositive)) - 1))
    : 1;
  const logBaseLabel = String(logBase).replace('.', spec.decimal);
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <div style={{position: 'absolute', left, top: wide ? 150 : 300, width: contentWidth, ...text(60), lineHeight: 1.15}}>{spec.title}</div>
      <div style={{position: 'absolute', left, top: wide ? 265 : 470, display: 'flex', gap: 36, ...(wide ? {width: contentWidth, flexWrap: 'wrap'} : {})}}>
        {spec.series.map((s) => (
          <span key={s.name} style={{display: 'flex', alignItems: 'center', gap: 12, ...text(36, s.color)}}>
            <span style={{width: 34, height: 34, borderRadius: 6, background: s.color}} />
            {s.name}
          </span>
        ))}
      </div>
      {spec.log && <div style={{position: 'absolute', left, top: wide ? 275 : 400, width: contentWidth, textAlign: 'right', ...text(24, COLOR.before, 600)}}>Логарифмическая шкала · от {logBaseLabel}{values.includes(0) ? ' · 0 — на базе' : ''}</div>}
      <div style={{position: 'absolute', left, top: bottom, width: contentWidth, height: 3, background: COLOR.grid}} />
      {spec.groups.map((g, gi) => {
        const x0 = left + gi * groupW + (groupW - barW * spec.series.length - 16 * (spec.series.length - 1)) / 2;
        const k = grow(t, g.at);
        return (
          <React.Fragment key={g.label}>
            {g.values.map((v, si) => {
              // Zero has no logarithm, so it stays on the explicitly labelled baseline.
              const h = spec.log
                ? (v > 0 ? (bottom - top) * Math.log(v / logBase) / Math.log(spec.max / logBase) * k : 0)
                : ((bottom - top) * v * k) / spec.max;
              const color = g.color ?? spec.series[si].color;
              const x = x0 + si * (barW + 16);
              return (
                <React.Fragment key={si}>
                  <div
                    style={{
                      position: 'absolute',
                      left: x,
                      top: bottom - h,
                      width: barW,
                      height: h,
                      background: color,
                      borderRadius: '8px 8px 0 0',
                    }}
                  />
                  <div
                    style={{
                      position: 'absolute',
                      left: x + barW / 2,
                      top: bottom - h - 56,
                      transform: 'translateX(-50%)',
                      opacity: k,
                      whiteSpace: 'nowrap',
                      ...text(36, color),
                    }}
                  >
                    {spec.format.replace('{v}', String(Number(v.toFixed(2))).replace('.', spec.decimal))}
                  </div>
                </React.Fragment>
              );
            })}
            <div
              style={{
                position: 'absolute',
                left: left + gi * groupW + groupW / 2,
                top: bottom + 18,
                transform: 'translateX(-50%)',
                opacity: shown(t, g.at),
                ...text(wide && hasNotes ? 32 : 44),
                ...(wide ? {width: groupW - 24, textAlign: 'center', lineHeight: 1.15} : {}),
              }}
            >
              {g.label}
              {g.note !== undefined && <div style={{marginTop: 7, ...text(24, COLOR.before, 600), lineHeight: 1.2}}>{g.note}</div>}
            </div>
          </React.Fragment>
        );
      })}
      <Source text={spec.source} />
    </AbsoluteFill>
  );
};

// marks filling one by one: the hits of all the shots
export const DotsView: React.FC<{spec: Dots}> = ({spec}) => {
  const t = useT();
  const {width, height} = useVideoConfig();
  const wide = width > height;
  const left = wide ? 220 : LEFT;
  const contentWidth = wide ? 1480 : WIDTH;
  const cols = wide ? Math.max(1, Math.min(10, spec.total)) : 6;
  const rows = Math.ceil(spec.total / cols);
  const size = wide ? Math.min(86, (360 - Math.max(0, rows - 1) * 22) / Math.max(1, rows)) : 96;
  const gap = cols > 1 ? (contentWidth - cols * size) / (cols - 1) : 0;
  const filled = Math.min(spec.hits, Math.floor(interpolate(t, [spec.at, spec.at + spec.dur], [0, spec.hits + 0.999], {
    extrapolateLeft: 'clamp',
    extrapolateRight: 'clamp',
  })));
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <div style={{position: 'absolute', left: wide ? 96 : LEFT, top: wide ? 150 : 300, width: wide ? 1728 : WIDTH, ...text(60), lineHeight: 1.15}}>{spec.title}</div>
      {Array.from({length: spec.total}, (_, i) => {
        const r = Math.floor(i / cols);
        const c = i % cols;
        const on = i < filled;
        return (
          <div
            key={i}
            style={{
              position: 'absolute',
              left: left + c * (size + gap),
              top: (wide ? 330 : 470) + r * (size + 22),
              width: size,
              height: size,
              borderRadius: size / 2,
              border: `5px solid ${on ? COLOR.now : COLOR.faint}`,
              background: on ? COLOR.now : 'transparent',
              opacity: shown(t, 0.1 + i * 0.01),
            }}
          />
        );
      })}
      <div style={{position: 'absolute', left: wide ? 96 : LEFT, top: wide ? 735 : 1080, width: wide ? 1728 : WIDTH, textAlign: 'center', ...text(72, COLOR.now)}}>
        {spec.caption.replace('{n}', String(filled))}
      </div>
      <Source text={spec.source} />
    </AbsoluteFill>
  );
};

// the character's numbers, then they grey out and a question stands over them
export const StatsView: React.FC<{spec: Stats}> = ({spec}) => {
  const t = useT();
  const {width, height} = useVideoConfig();
  const wide = width > height;
  const grey = interpolate(t, [spec.fadeAt, spec.fadeAt + 0.5], [1, 0.3], {
    extrapolateLeft: 'clamp',
    extrapolateRight: 'clamp',
  });
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <div
        style={{
          position: 'absolute',
          left: wide ? 304 : LEFT,
          top: wide ? 200 : 380,
          width: wide ? 1312 : WIDTH,
          ...(wide ? {boxSizing: 'border-box' as const} : {}),
          padding: '34px 40px',
          borderRadius: 18,
          border: `2px solid ${COLOR.grid}`,
          opacity: grey,
          filter: `grayscale(${1 - grey})`,
        }}
      >
        {spec.rows.map((row, i) => (
          <div
            key={i}
            style={{
              display: 'flex',
              justifyContent: 'space-between',
              padding: '14px 0',
              opacity: shown(t, 0.2 + i * 0.25),
              ...text(wide ? Math.min(60, Math.max(36, (430 / Math.max(1, spec.rows.length) - 28) / 1.2)) : 64, row.color ?? COLOR.text),
              ...(wide ? {gap: 40, lineHeight: 1.2} : {}),
            }}
          >
            <span>{row.name}</span>
            <span>{row.value}</span>
          </div>
        ))}
      </div>
      <div
        style={{
          position: 'absolute',
          left: wide ? 96 : LEFT,
          top: wide ? 755 : 860,
          width: wide ? 1728 : WIDTH,
          textAlign: 'center',
          opacity: shown(t, spec.question.at),
          transform: `translateY(${rise(t, spec.question.at, 20)}px)`,
          ...text(72, COLOR.now),
          fontFamily: FONT.title,
        }}
      >
        {spec.question.text}
      </div>
    </AbsoluteFill>
  );
};
