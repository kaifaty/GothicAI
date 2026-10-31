import React from 'react';
import {AbsoluteFill, useCurrentFrame, useVideoConfig} from 'remotion';
import type {ConeCard, FlowCard, GridCard, RaysCard, TicksCard} from '../schema';
import {COLOR, FONT, FRAME, OUTLINE, WIDE_SAFE} from '../theme';
import {shown} from './anim';
import {clamp01, inCone, interpolatePath, quadraticParameterAtLength, quadraticPoint} from './diagramMath';

const {width: W, height: H} = FRAME.wide;
const LEFT = WIDE_SAFE.left;
const WIDTH = W - LEFT - WIDE_SAFE.right;
const useT = () => useCurrentFrame() / useVideoConfig().fps;
const text = (size: number, color: string = COLOR.text, weight = 700): React.CSSProperties => ({
  fontFamily: FONT.body, fontSize: size, fontWeight: weight, color,
  fontVariantNumeric: 'tabular-nums', textShadow: OUTLINE,
});
const Title: React.FC<{children?: React.ReactNode}> = ({children}) => children ? (
  <div style={{position: 'absolute', left: LEFT, top: 140, width: WIDTH, ...text(60), fontFamily: FONT.title, lineHeight: 1.15}}>{children}</div>
) : null;
const Source: React.FC<{source: string}> = ({source}) => (
  <div style={{position: 'absolute', left: LEFT, top: 850, width: WIDTH, ...text(25, COLOR.before, 600), lineHeight: 1.35, overflowWrap: 'anywhere'}}>{source}</div>
);
const DiagramSvg: React.FC<{children: React.ReactNode}> = ({children}) => (
  <svg width={W} height={H} style={{position: 'absolute', inset: 0, fontFamily: FONT.body, fontVariantNumeric: 'tabular-nums'}}>{children}</svg>
);
const Fighter: React.FC<{x: number; y: number; deg?: number}> = ({x, y, deg = 0}) => (
  <g transform={`translate(${x} ${y})`}>
    <path d="M 0 -40 L -12 -20 L 12 -20 Z" fill={COLOR.now} transform={`rotate(${deg})`} />
    <circle r={21} fill={COLOR.now} stroke={COLOR.card} strokeWidth={5} />
    <text y={58} textAnchor="middle" fill={COLOR.now} fontSize={29} fontWeight={800}>я</text>
  </g>
);

export const RaysView: React.FC<{spec: RaysCard}> = ({spec}) => {
  const t = useT();
  const cx = 525, cy = 530, radius = 264;
  const scale = radius / spec.maxCm;
  const hidden = spec.hideAt !== undefined && t >= spec.hideAt;
  const targetOpacity = hidden ? 0 : shown(t, spec.target.at);
  const panelGap = 26;
  const panelHeight = Math.min(240, (500 - panelGap * (spec.panels.length - 1)) / spec.panels.length);
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      <DiagramSvg>
        <circle cx={cx} cy={cy} r={radius} fill="none" stroke={COLOR.grid} strokeWidth={2} strokeDasharray="5 10" />
        <text x={cx} y={cy - radius - 22} textAnchor="middle" fill={COLOR.before} fontSize={24} fontWeight={600}>вперёд</text>
        {spec.rays.map((ray, i) => {
          const angle = i * 2 * Math.PI / 16;
          const length = ray.hit ? ray.cm * scale : radius;
          const progress = clamp01((t - spec.raysAt[0]) / spec.raysAt[1] * 16 - i);
          const x = cx + Math.sin(angle) * length;
          const y = cy - Math.cos(angle) * length;
          return (
            <g key={i} opacity={progress}>
              <line x1={cx} y1={cy} x2={cx + (x - cx) * progress} y2={cy + (y - cy) * progress} stroke={ray.hit ? COLOR.miss : COLOR.before} strokeWidth={4} />
              {ray.hit && <circle cx={x} cy={y} r={6} fill={COLOR.miss} opacity={progress} />}
            </g>
          );
        })}
        <g opacity={targetOpacity}>
          <circle cx={cx + spec.target.x * scale} cy={cy - spec.target.y * scale} r={18} fill={COLOR.skill} stroke={COLOR.card} strokeWidth={4} />
          <text x={cx + spec.target.x * scale + 27} y={cy - spec.target.y * scale + 10} fill={COLOR.skill} fontSize={28} fontWeight={700}>{spec.target.label}</text>
        </g>
        <Fighter x={cx} y={cy} />
      </DiagramSvg>
      {spec.panels.map((panel, i) => {
        const noData = i === 1 && hidden;
        const fade = noData && spec.hideAt !== undefined ? clamp01((t - spec.hideAt) / 0.3) : 0;
        return (
          <div key={i} style={{position: 'absolute', left: 1000, top: 285 + i * (panelHeight + panelGap), width: 740, height: panelHeight, boxSizing: 'border-box', padding: '24px 30px', borderRadius: 16, border: `2px solid ${noData ? COLOR.faint : panel.color}`, opacity: shown(t, panel.at)}}>
            <div style={{...text(34, panel.color), opacity: 1 - fade * 0.65}}>{panel.title}</div>
            <div style={{marginTop: 16, opacity: 1 - fade, ...text(Math.min(31, (panelHeight - 90) / Math.max(1, panel.lines.length) / 1.4), COLOR.before, 600), lineHeight: 1.4}}>
              {panel.lines.map((line, li) => <div key={li}>{line}</div>)}
            </div>
            {noData && <div style={{position: 'absolute', left: 30, top: 90, opacity: fade, ...text(35, COLOR.before)}}>нет данных</div>}
          </div>
        );
      })}
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};

export const ConeView: React.FC<{spec: ConeCard}> = ({spec}) => {
  const t = useT();
  const cx = 550, cy = 585, radius = 295;
  const extent = Math.max(spec.rangeCm, ...spec.path.map((p) => Math.hypot(p.x, p.y)));
  const scale = radius / extent;
  const r = spec.rangeCm * scale;
  const half = spec.halfDeg * Math.PI / 180;
  const turn = spec.turn;
  const deg = turn ? turn.deg * clamp01((t - turn.at) / turn.dur) : 0;
  const target = interpolatePath(spec.path, t);
  const visible = inCone(target, spec.halfDeg, spec.rangeCm, deg);
  const targetOpacity = shown(t, spec.path[0].t) * (visible ? 1 : 0.22);
  const tx = cx + target.x * scale, ty = cy - target.y * scale;
  const sector = `M 0 0 L ${-Math.sin(half) * r} ${-Math.cos(half) * r} A ${r} ${r} 0 ${spec.halfDeg > 90 ? 1 : 0} 1 ${Math.sin(half) * r} ${-Math.cos(half) * r} Z`;
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      <DiagramSvg>
        <g transform={`translate(${cx} ${cy}) rotate(${deg})`}>
          <path d={sector} fill={COLOR.now} fillOpacity={0.09} stroke={COLOR.now} strokeWidth={3} />
          <line y2={-r} stroke={COLOR.now} strokeOpacity={0.3} strokeWidth={2} strokeDasharray="6 10" />
        </g>
        <g opacity={targetOpacity}>
          <circle cx={tx} cy={ty} r={21} fill={COLOR.skill} />
          <text x={tx + 32} y={ty + 10} fill={COLOR.skill} fontSize={29} fontWeight={700}>{spec.targetLabel}</text>
        </g>
        <Fighter x={cx} y={cy} deg={deg} />
        {turn && <text x={cx - 165} y={780} fill={COLOR.now} fontSize={34} fontWeight={800} opacity={shown(t, turn.at)}>{turn.label}</text>}
      </DiagramSvg>
      <div style={{position: 'absolute', left: 1040, top: 315, width: 720}}>
        {spec.texts.map((line, i) => <div key={i} style={{marginBottom: 36, ...text(38, line.color), lineHeight: 1.35, opacity: shown(t, line.at)}}>{line.text}</div>)}
      </div>
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};

export const TicksView: React.FC<{spec: TicksCard}> = ({spec}) => {
  const t = useT();
  const aligned = spec.align ? clamp01((t - spec.align.at) / spec.align.dur) : 0;
  const firstOffset = spec.rows[0].offset;
  const columns = Math.max(...spec.rows.map((row) => row.cells.length + Math.max(row.offset, firstOffset)));
  const pitch = Math.min(92, 1450 / columns);
  const cellWidth = pitch - Math.min(10, pitch * 0.15);
  const rowHeight = Math.min(150, 390 / spec.rows.length);
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      {spec.rows.map((row, i) => (
        <div key={i} style={{position: 'absolute', left: LEFT, top: 370 + i * rowHeight, opacity: shown(t, row.at)}}>
          <div style={{position: 'absolute', top: 21, width: 225, ...text(36, COLOR.before)}}>{row.label}</div>
          <div style={{position: 'absolute', left: 255 + (row.offset + (firstOffset - row.offset) * aligned) * pitch, top: 0, display: 'flex', gap: pitch - cellWidth}}>
            {[...row.cells].map((cell, ci) => {
              const color = cell === 'D' ? COLOR.now : COLOR.before;
              const progress = clamp01((t - row.at) / spec.dur * row.cells.length - ci);
              return <div key={ci} style={{width: cellWidth, height: 90, borderRadius: 7, background: color, opacity: progress}} />;
            })}
          </div>
        </div>
      ))}
      <div style={{position: 'absolute', left: LEFT + 255, top: 370 + spec.rows.length * rowHeight + 55, display: 'flex', gap: 70, opacity: shown(t, Math.min(...spec.rows.map((r) => r.at)))}}>
        {(['D', 'B'] as const).map((cell) => <div key={cell} style={{display: 'flex', gap: 16, alignItems: 'center', ...text(30, cell === 'D' ? COLOR.now : COLOR.before, 600)}}><span style={{width: 32, height: 32, borderRadius: 4, background: cell === 'D' ? COLOR.now : COLOR.before}} /><span>{spec.legend[cell]}</span></div>)}
      </div>
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};

export const GridView: React.FC<{spec: GridCard}> = ({spec}) => {
  const t = useT();
  const rows = Math.ceil(spec.tiles.length / spec.columns);
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      <div style={{position: 'absolute', left: LEFT, top: 300, width: WIDTH, height: 470, display: 'grid', gridTemplateColumns: `repeat(${spec.columns}, 1fr)`, gridTemplateRows: `repeat(${rows}, 1fr)`, gap: 20}}>
        {spec.tiles.map((tile, i) => {
          const on = shown(t, tile.at);
          return (
            <div key={i} style={{position: 'relative', border: `2px solid ${COLOR.faint}`, borderRadius: 13, overflow: 'hidden'}}>
              <div style={{position: 'absolute', inset: 0, borderRadius: 11, border: `2px solid ${COLOR.now}`, background: `${COLOR.now}20`, opacity: on}} />
              <div style={{position: 'absolute', inset: 0, padding: 15, display: 'flex', alignItems: 'center', justifyContent: 'center', textAlign: 'center', ...text(Math.min(36, WIDTH / spec.columns / 10), COLOR.now), opacity: on, lineHeight: 1.25}}>{tile.label}</div>
            </div>
          );
        })}
      </div>
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};

type FlowNode = FlowCard['nodes'][number];
const box = (node: FlowNode) => ({x: node.x * W, y: node.y * H, w: (node.w ?? 0.2) * W, h: 126});
// Intersect the tangent with the node rectangle, so arrows touch the box rather than its centre.
const boundary = (node: ReturnType<typeof box>, toward: {x: number; y: number}) => {
  const dx = toward.x - node.x, dy = toward.y - node.y;
  const k = Math.min(node.w / 2 / (Math.abs(dx) || 1e-9), node.h / 2 / (Math.abs(dy) || 1e-9));
  return {x: node.x + dx * k, y: node.y + dy * k};
};

export const FlowView: React.FC<{spec: FlowCard}> = ({spec}) => {
  const t = useT();
  const nodes = new Map(spec.nodes.map((node) => [node.id, node]));
  return (
    <AbsoluteFill style={{background: COLOR.card}}>
      <Title>{spec.title}</Title>
      <DiagramSvg>
        {spec.edges.map((edge, i) => {
          const from = nodes.get(edge.from)!, to = nodes.get(edge.to)!;
          const a = box(from), b = box(to);
          const dx = b.x - a.x, dy = b.y - a.y, length = Math.hypot(dx, dy) || 1;
          const bend = (edge.curve ?? 0) * H;
          const c = {x: (a.x + b.x) / 2 - dy / length * bend, y: (a.y + b.y) / 2 + dx / length * bend};
          const start = boundary(a, c), end = boundary(b, c);
          const at = edge.at;
          const progress = clamp01((t - at) / 0.6);
          const tipT = quadraticParameterAtLength(start, c, end, progress);
          const tip = quadraticPoint(start, c, end, tipT);
          const tangent = {x: (1 - tipT) * (c.x - start.x) + tipT * (end.x - c.x), y: (1 - tipT) * (c.y - start.y) + tipT * (end.y - c.y)};
          const angle = Math.atan2(tangent.y, tangent.x) * 180 / Math.PI;
          const label = quadraticPoint(start, c, end, 0.5);
          const packets = edge.packets;
          const elapsed = t - at - 0.6;
          const packetCount = packets ? Math.ceil(packets.dur / packets.every) : 0;
          return (
            <g key={i} opacity={shown(t, at)}>
              <path d={`M ${start.x} ${start.y} Q ${c.x} ${c.y} ${end.x} ${end.y}`} fill="none" stroke={COLOR.now} strokeWidth={4} pathLength={1} strokeDasharray={1} strokeDashoffset={1 - progress} />
              {progress > 0 && <path d="M 0 0 L -17 -8 L -17 8 Z" fill={COLOR.now} transform={`translate(${tip.x} ${tip.y}) rotate(${angle})`} />}
              {edge.label && <text x={label.x} y={label.y - 20} textAnchor="middle" fill={COLOR.before} stroke={COLOR.card} strokeWidth={9} paintOrder="stroke" fontSize={28} fontWeight={600}>{edge.label}</text>}
              {packets && elapsed >= 0 && Array.from({length: packetCount}, (_, pi) => {
                // Keep only the most recent duration of launches, independent of the card's length.
                const launch = Math.floor(elapsed / packets.every) - pi;
                const age = elapsed - launch * packets.every;
                if (launch < 0 || age < 0 || age >= packets.dur) return null;
                const p = quadraticPoint(start, c, end, age / packets.dur);
                return <circle key={pi} cx={p.x} cy={p.y} r={7} fill={COLOR.now} stroke={COLOR.card} strokeWidth={2} />;
              })}
            </g>
          );
        })}
      </DiagramSvg>
      {spec.nodes.map((node) => {
        const b = box(node);
        const color = node.color ?? COLOR.now;
        return (
          <div key={node.id} style={{position: 'absolute', left: b.x - b.w / 2, top: b.y - b.h / 2, width: b.w, height: b.h, boxSizing: 'border-box', border: `3px solid ${color}`, borderRadius: 15, background: COLOR.card, padding: '15px 20px', display: 'flex', alignItems: 'center', justifyContent: 'center', textAlign: 'center', whiteSpace: 'pre-line', lineHeight: 1.3, ...text(Math.min(36, b.w / 10), color), opacity: shown(t, node.at)}}>{node.label}</div>
        );
      })}
      <Source source={spec.source} />
    </AbsoluteFill>
  );
};
