import React, {useId} from 'react';
import {AbsoluteFill, useCurrentFrame, useVideoConfig} from 'remotion';
import type {BigCard, CellsCard, LineCard, TableCard} from '../schema';
import {COLOR, FONT, FRAME, OUTLINE, WIDE_SAFE} from '../theme';
import {rise, shown} from './anim';

const LEFT = WIDE_SAFE.left;
const WIDTH = FRAME.wide.width - WIDE_SAFE.left - WIDE_SAFE.right;
const clamp = (v: number, min: number, max: number) => Math.max(min, Math.min(max, v));
const useT = () => useCurrentFrame() / useVideoConfig().fps;
const text = (size: number, color: string = COLOR.text, weight = 700): React.CSSProperties => ({
  fontFamily: FONT.body,
  fontSize: size,
  fontWeight: weight,
  color,
  fontVariantNumeric: 'tabular-nums',
  textShadow: OUTLINE,
});
const number = (value: number, digits?: number) => new Intl.NumberFormat('ru-RU', {
  minimumFractionDigits: digits ?? 0,
  maximumFractionDigits: digits ?? 6,
}).format(value);

const Title: React.FC<{children?: React.ReactNode}> = ({children}) => children ? (
  <div style={{position: 'absolute', left: LEFT, top: 140, width: WIDTH, ...text(60), fontFamily: FONT.title, lineHeight: 1.15}}>{children}</div>
) : null;

const Source: React.FC<{source: string}> = ({source}) => (
  <div style={{position: 'absolute', left: LEFT, top: 850, width: WIDTH, ...text(25, COLOR.before, 600), lineHeight: 1.35, overflowWrap: 'anywhere'}}>{source}</div>
);

// Reveal by horizontal position rather than path length: every X receives the same share of the duration.
export const LineView: React.FC<{spec: LineCard}> = ({spec}) => {
  const t = useT();
  const clipId = useId().replace(/:/g, '');
  const plot = {left: LEFT + 118, right: FRAME.wide.width - WIDE_SAFE.right - 72, top: 350, bottom: 710};
  const x = (v: number) => plot.left + (v - spec.axisX.min) / (spec.axisX.max - spec.axisX.min) * (plot.right - plot.left);
  const y = (v: number) => plot.bottom - (v - spec.axisY.min) / (spec.axisY.max - spec.axisY.min) * (plot.bottom - plot.top);
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      <div style={{position: 'absolute', left: plot.left, top: 244, width: plot.right - plot.left, display: 'flex', flexWrap: 'wrap', gap: '14px 38px'}}>
        {spec.series.map((series, i) => (
          <div key={i} style={{display: 'flex', alignItems: 'center', gap: 12, opacity: shown(t, series.draw[0]), ...text(30, series.color)}}>
            <span style={{width: 42, height: 5, background: series.color, borderRadius: 3}} />
            {series.name}
          </div>
        ))}
      </div>
      <svg width={FRAME.wide.width} height={FRAME.wide.height} style={{position: 'absolute', inset: 0, fontFamily: FONT.body, fontVariantNumeric: 'tabular-nums'}}>
        <text x={plot.left} y={320} fill={COLOR.before} fontSize={27} fontWeight={600}>{spec.axisY.label}</text>
        {spec.axisY.ticks.map((tick, i) => (
          <g key={i}>
            <line x1={plot.left} x2={plot.right} y1={y(tick)} y2={y(tick)} stroke={COLOR.grid} strokeWidth={2} />
            <text x={plot.left - 20} y={y(tick)} textAnchor="end" dominantBaseline="middle" fill={COLOR.before} fontSize={28} fontWeight={600}>{number(tick)}</text>
          </g>
        ))}
        {spec.axisX.ticks.map((tick, i) => (
          <g key={i}>
            <line x1={x(tick)} x2={x(tick)} y1={plot.top} y2={plot.bottom} stroke={COLOR.grid} strokeWidth={2} />
            <text x={x(tick)} y={plot.bottom + 44} textAnchor="middle" fill={COLOR.before} fontSize={28} fontWeight={600}>{number(tick)}</text>
          </g>
        ))}
        <path d={`M ${plot.left} ${plot.top} V ${plot.bottom} H ${plot.right}`} fill="none" stroke={COLOR.before} strokeWidth={3} />
        <text x={(plot.left + plot.right) / 2} y={810} textAnchor="middle" fill={COLOR.before} fontSize={29} fontWeight={600}>{spec.axisX.label}</text>
        {spec.marks.map((mark, i) => {
          const anchor = x(mark.x) > plot.right - 240 ? 'end' : x(mark.x) < plot.left + 240 ? 'start' : 'middle';
          return (
            <g key={i} opacity={shown(t, mark.at)}>
              <line x1={x(mark.x)} x2={x(mark.x)} y1={plot.top} y2={plot.bottom} stroke={COLOR.now} strokeWidth={3} strokeDasharray="9 9" />
              <text x={x(mark.x)} y={plot.top - 19} textAnchor={anchor} fill={COLOR.now} stroke={COLOR.card} strokeWidth={8} paintOrder="stroke" fontSize={27} fontWeight={800}>{mark.text}</text>
            </g>
          );
        })}
        {spec.series.map((series, i) => {
          const progress = clamp((t - series.draw[0]) / series.draw[1], 0, 1);
          const start = x(series.points[0][0]);
          const end = x(series.points[series.points.length - 1][0]);
          const edge = start + (end - start) * progress;
          return (
            <React.Fragment key={i}>
              <defs>
                <clipPath id={`${clipId}-${i}`}>
                  <rect x={start - 4} y={plot.top - 5} width={progress > 0 ? edge - start + (progress === 1 ? 8 : 4) : 0} height={plot.bottom - plot.top + 10} />
                </clipPath>
              </defs>
              <g clipPath={`url(#${clipId}-${i})`} opacity={t >= series.draw[0] ? 1 : 0}>
                <polyline points={series.points.map(([px, py]) => `${x(px)},${y(py)}`).join(' ')} fill="none" stroke={series.color} strokeWidth={6} strokeLinecap="round" strokeLinejoin="round" />
                {series.points.map(([px, py], pi) => <circle key={pi} cx={x(px)} cy={y(py)} r={4} fill={series.color} />)}
              </g>
            </React.Fragment>
          );
        })}
        {spec.labels.map((label, i) => {
          const width = clamp(label.text.length * 16 + 28, 100, 420);
          const lx = clamp(x(label.x) + 15, plot.left + 10, plot.right - width - 10);
          const ly = clamp(y(label.y) < plot.top + 65 ? y(label.y) + 16 : y(label.y) - 55, plot.top + 12, plot.bottom - 48);
          return (
            <g key={i} opacity={shown(t, label.at)}>
              <circle cx={x(label.x)} cy={y(label.y)} r={7} fill={COLOR.now} stroke={COLOR.card} strokeWidth={3} />
              <rect x={lx} y={ly} width={width} height={44} rx={7} fill={COLOR.card} stroke={COLOR.grid} />
              <text x={lx + 14} y={ly + 30} fill={COLOR.text} fontSize={26} fontWeight={700}>{label.text}</text>
            </g>
          );
        })}
      </svg>
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};

export const BigView: React.FC<{spec: BigCard}> = ({spec}) => {
  const t = useT();
  const itemWidth = WIDTH / spec.items.length;
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      {spec.items.map((item, i) => {
        const count = item.count;
        const value = count
          ? count.format.replace(/\{v(?::([1-6]))?\}/g, (_, digits: string | undefined) => number(count.from + (count.to - count.from) * clamp((t - item.at) / count.dur, 0, 1), digits ? Number(digits) : 0))
          : typeof item.value === 'number' ? number(item.value) : item.value;
        const fontSize = Math.min(spec.items.length === 1 ? 172 : spec.items.length === 2 ? 142 : 112, (itemWidth - 70) / Math.max(value.length * 0.62, 1));
        return (
          <div key={i} style={{position: 'absolute', left: LEFT + i * itemWidth + 24, top: 350, width: itemWidth - 48, textAlign: 'center', opacity: shown(t, item.at), transform: `translateY(${rise(t, item.at, 18)}px)`}}>
            <div style={{...text(fontSize, item.color ?? COLOR.now, 800), whiteSpace: 'nowrap', lineHeight: 1.25}}>{value}</div>
            <div style={{marginTop: 32, ...text(spec.items.length === 3 ? 36 : 42, COLOR.before, 600), lineHeight: 1.3}}>{item.label}</div>
          </div>
        );
      })}
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};

export const TableView: React.FC<{spec: TableCard}> = ({spec}) => {
  const t = useT();
  const rowHeight = Math.min(126, 430 / spec.rows.length);
  const fontSize = Math.min(42, Math.max(22, rowHeight * 0.37), 180 / spec.columns.length);
  const columns = spec.columns.map((_, i) => i === 0 && spec.columns.length > 1 ? '1.25fr' : '1fr').join(' ');
  const cellStyle = (i: number): React.CSSProperties => ({
    display: 'flex',
    alignItems: 'center',
    justifyContent: i === 0 ? 'flex-start' : 'center',
    minWidth: 0,
    padding: '12px 28px',
    boxSizing: 'border-box',
    color: i === spec.highlight ? COLOR.now : COLOR.text,
    background: i === spec.highlight ? `${COLOR.now}14` : 'transparent',
    lineHeight: 1.25,
    overflowWrap: 'anywhere',
  });
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      <div style={{position: 'absolute', left: LEFT, top: 320, width: WIDTH, ...text(fontSize)}}>
        <div style={{display: 'grid', gridTemplateColumns: columns, minHeight: 92, borderBottom: `3px solid ${COLOR.grid}`, ...text(Math.min(40, fontSize), COLOR.before, 800)}}>
          {spec.columns.map((column, i) => <div key={i} style={{...cellStyle(i), color: i === spec.highlight ? COLOR.now : COLOR.before}}>{column}</div>)}
        </div>
        {spec.rows.map((row, i) => (
          <div key={i} style={{display: 'grid', gridTemplateColumns: columns, minHeight: rowHeight, borderBottom: `2px solid ${COLOR.grid}`, opacity: shown(t, row.at)}}>
            {row.cells.map((cell, ci) => <div key={ci} style={cellStyle(ci)}>{typeof cell === 'number' ? number(cell) : cell}</div>)}
          </div>
        ))}
      </div>
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};

export const CellsView: React.FC<{spec: CellsCard}> = ({spec}) => {
  const t = useT();
  const gap = 14;
  const width = (WIDTH - gap * (spec.cells.length - 1)) / spec.cells.length;
  const cellX = (i: number) => LEFT + i * (width + gap);
  const cellTop = 420;
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      {spec.spans.map((span, i) => {
        const left = cellX(span.from);
        const right = cellX(span.to) + width;
        return (
          <div key={i} style={{position: 'absolute', left, top: 330, width: right - left, height: 66, opacity: shown(t, spec.cells[span.from].at), textAlign: 'center', ...text(32, COLOR.before, 600)}}>
            {span.text}
            <div style={{position: 'absolute', left: 0, right: 0, bottom: 0, height: 11, border: `2px solid ${COLOR.faint}`, borderBottom: 0}} />
          </div>
        );
      })}
      {spec.cells.map((cell, i) => {
        const color = cell.state === 'fail' ? COLOR.miss : cell.state === 'pass' ? COLOR.dexterity : COLOR.before;
        const symbol = cell.state === 'fail' ? '×' : cell.state === 'pass' ? '✓' : '—';
        return (
          <div key={i} style={{position: 'absolute', left: cellX(i), top: cellTop, width, height: 154, boxSizing: 'border-box', border: `3px solid ${color}`, borderRadius: 10, background: `${color}14`, textAlign: 'center', opacity: shown(t, cell.at), transform: `translateY(${rise(t, cell.at, 12)}px)`}}>
            <div style={{marginTop: 19, ...text(Math.min(30, width * 0.28)), whiteSpace: 'nowrap'}}>{cell.label}</div>
            <div style={{marginTop: 7, ...text(Math.min(66, width * 0.63), color, 800), lineHeight: 1}}>{symbol}</div>
          </div>
        );
      })}
      {spec.notes.map((note, i) => <div key={i} style={{position: 'absolute', left: LEFT, top: 625 + i * 62, width: WIDTH, opacity: shown(t, note.at), ...text(34, note.color ?? COLOR.before, 600), lineHeight: 1.3}}>{note.text}</div>)}
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};
