import React from 'react';
import {
  AbsoluteFill,
  Audio,
  Freeze,
  Img,
  OffthreadVideo,
  Sequence,
  interpolate,
  staticFile,
  useCurrentFrame,
  useVideoConfig,
} from 'remotion';
import './fonts';
import {BrandSting, STING_FRAMES} from './Brand';
import {Captions} from './parts/Captions';
import {BarsView, DotsView, FormulaView, StatsView} from './parts/Cards';
import {BigView, CellsView, LineView, TableView} from './parts/WideCards';
import {DiceChart} from './parts/DiceChart';
import {Sight} from './parts/Sight';
import {Tags} from './parts/Tags';
import {Hook, Texts} from './parts/Texts';
import type {Beat, Plan} from './schema';
import {COLOR, FONT, OUTLINE, WIDE_SAFE} from './theme';

// the voice speaks in these spans (seconds of the short): the game ducks under them
type Spans = [number, number][];

const ducked = (t: number, spans: Spans, duck: number, ramp = 0.15) => {
  let k = 1;
  for (const [a, b] of spans) {
    // ramps either side of a line
    const v = interpolate(t, [a - ramp, a, b, b + ramp], [1, duck, duck, 1], {
      extrapolateLeft: 'clamp',
      extrapolateRight: 'clamp',
    });
    k = Math.min(k, v);
  }
  return k;
};

// a picture filling the frame: footage (frozen from `freeze` on) or a still, with an optional slow zoom
const Picture: React.FC<{
  src: string;
  from: number;
  beat: Beat;
  width: number;
  height: number;
  volume: (f: number) => number;
}> = ({src, from, beat, width, height, volume}) => {
  const {fps} = useVideoConfig();
  const t = useCurrentFrame() / fps;
  const z = beat.zoom;
  const s = z
    ? interpolate(t, [z.at, z.at + z.dur], [1, z.scale], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'})
    : 1;
  const style: React.CSSProperties = {
    width,
    height,
    objectFit: 'cover',
    transform: `scale(${s})`,
    transformOrigin: z ? `${z.x}px ${z.y}px` : 'center',
  };
  if (src.endsWith('.png') || src.endsWith('.jpg')) {
    return <Img src={staticFile(src)} style={style} />;
  }
  const video = <OffthreadVideo src={staticFile(src)} startFrom={Math.round(from * fps)} volume={volume} style={style} />;
  if (beat.freeze !== undefined && t >= beat.freeze) {
    return <Freeze frame={Math.round(beat.freeze * fps)}>{video}</Freeze>;
  }
  return video;
};

const HalfLabel: React.FC<{text: string; top: number; color: string; left?: number; maxWidth?: number}> = ({text, top, color, left = 40, maxWidth}) => (
  <div
    style={{
      position: 'absolute',
      top,
      left,
      maxWidth,
      boxSizing: maxWidth ? 'border-box' : undefined,
      overflowWrap: maxWidth ? 'anywhere' : undefined,
      fontFamily: FONT.body,
      fontWeight: 800,
      fontSize: 40,
      color,
      background: COLOR.shade,
      padding: '6px 16px',
      borderRadius: 10,
      textShadow: OUTLINE,
    }}
  >
    {text}
  </div>
);

const BeatView: React.FC<{beat: Beat; start: number; spans: Spans; duck: number; game: number; stem?: Plan['stem']}> = ({
  beat,
  start,
  spans,
  duck,
  game,
  stem,
}) => {
  const {fps, width, height} = useVideoConfig();
  const wide = width > height;
  const volume = (f: number) =>
    stem ? (stem === 'game' ? beat.game : 0) : game * beat.game * ducked(start + f / fps, spans, duck);
  let body: React.ReactNode = null;
  const card = beat.chart;
  if (beat.kind === 'card' && card) {
    if (card.type === 'dice') body = <DiceChart spec={card} />;
    else if (card.type === 'formula') body = <FormulaView spec={card} />;
    else if (card.type === 'bars') body = <BarsView spec={card} />;
    else if (card.type === 'dots') body = <DotsView spec={card} />;
    else if (card.type === 'stats') body = <StatsView spec={card} />;
    else if (card.type === 'line') body = <LineView spec={card} />;
    else if (card.type === 'big') body = <BigView spec={card} />;
    else if (card.type === 'table') body = <TableView spec={card} />;
    else if (card.type === 'cells') body = <CellsView spec={card} />;
  } else if (beat.kind === 'split' && beat.src && beat.srcBottom) {
    const half = height / 2;
    body = wide ? (
      <>
        <div style={{position: 'absolute', left: 0, width: width / 2, height, overflow: 'hidden'}}>
          <Picture src={beat.src} from={beat.from} beat={beat} width={width / 2} height={height} volume={() => 0} />
        </div>
        <div style={{position: 'absolute', left: width / 2, width: width / 2, height, overflow: 'hidden'}}>
          <Picture src={beat.srcBottom} from={beat.fromBottom} beat={beat} width={width / 2} height={height} volume={volume} />
        </div>
        <div style={{position: 'absolute', left: width / 2 - 2, width: 4, height, background: '#000'}} />
        {beat.labels && beat.labels.map((label, i) => (
          <HalfLabel key={i} text={label} top={WIDE_SAFE.top} left={i * width / 2 + WIDE_SAFE.left}
            maxWidth={width / 2 - 2 * WIDE_SAFE.left} color={beat.labelColors?.[i] ?? (i === 0 ? COLOR.before : COLOR.now)} />
        ))}
      </>
    ) : (
      <>
        <div style={{position: 'absolute', top: 0, width, height: half, overflow: 'hidden'}}>
          <Picture src={beat.src} from={beat.from} beat={beat} width={width} height={half} volume={() => 0} />
        </div>
        <div style={{position: 'absolute', top: half, width, height: half, overflow: 'hidden'}}>
          <Picture src={beat.srcBottom} from={beat.fromBottom} beat={beat} width={width} height={half} volume={volume} />
        </div>
        <div style={{position: 'absolute', top: half - 2, width, height: 4, background: '#000'}} />
        {beat.labels && (
          <>
            <HalfLabel text={beat.labels[0]} top={170} color={beat.labelColors?.[0] ?? COLOR.before} />
            <HalfLabel text={beat.labels[1]} top={half + 40} color={beat.labelColors?.[1] ?? COLOR.now} />
          </>
        )}
      </>
    );
  } else if (beat.src) {
    body = <Picture src={beat.src} from={beat.from} beat={beat} width={width} height={height} volume={volume} />;
  }
  return (
    <AbsoluteFill style={{background: '#000', overflow: 'hidden'}}>
      {body}
      <Sight strokes={beat.strokes} fans={beat.fans} />
      <Tags tags={beat.tags} />
      <Texts texts={beat.texts} />
    </AbsoluteFill>
  );
};

// the music bed: under the beats, fading in and out, lower while the voice speaks (slow ramps)
const Music: React.FC<{music: NonNullable<Plan['music']>; spans: Spans; frames: number; stem?: boolean}> = ({
  music,
  spans,
  frames,
  stem,
}) => {
  const {fps} = useVideoConfig();
  const end = frames / fps;
  const volume = (f: number) => {
    const t = f / fps;
    const fade = Math.min(
      interpolate(t, [0, music.fadeIn], [0, 1], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'}),
      interpolate(t, [end - music.fadeOut, end], [1, 0], {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'}),
    );
    return stem ? fade : music.volume * fade * ducked(t, spans, music.duck, 0.35);
  };
  return <Audio src={staticFile(music.file)} volume={volume} loop loopVolumeCurveBehavior={stem ? 'extend' : 'repeat'} />;
};

export const Explainer: React.FC<Plan> = (plan) => {
  const {fps} = useVideoConfig();
  const contentFrames = Math.round(plan.beats.reduce((s, b) => s + b.dur, 0) * fps);
  const spans: Spans = plan.lines.filter((l) => l.length > 0).map((l) => [l[0].start, l[l.length - 1].end]);
  let t = 0;
  return (
    <AbsoluteFill style={{background: '#000'}}>
      <Sequence durationInFrames={contentFrames} layout="none" name="beats">
        {plan.beats.map((beat) => {
          const from = Math.round(t * fps);
          const start = t;
          t += beat.dur;
          return (
            <Sequence key={beat.id} from={from} durationInFrames={Math.round(t * fps) - from} name={beat.id}>
              <BeatView beat={beat} start={start} spans={spans} duck={plan.duck} game={plan.gameVolume} stem={plan.stem} />
            </Sequence>
          );
        })}
        {plan.music && (!plan.stem || plan.stem === 'music') && (
          <Music music={plan.music} spans={spans} frames={contentFrames} stem={plan.stem === 'music'} />
        )}
        {(!plan.stem || plan.stem === 'voice') &&
          plan.voice.map((v) => (
            <Sequence key={v.file} from={Math.round(v.start * fps)} name={v.file}>
              <Audio src={staticFile(v.file)} />
            </Sequence>
          ))}
        <Captions lines={plan.lines} brand={plan.brand} />
        <Hook text={plan.hook.text} until={plan.hook.until} />
      </Sequence>
      {plan.brand && (
        <Sequence from={contentFrames} durationInFrames={STING_FRAMES} name="brand sting">
          <BrandSting volume={plan.stem ? (plan.stem === 'game' ? 1 : 0) : plan.brandVolume} />
        </Sequence>
      )}
    </AbsoluteFill>
  );
};
