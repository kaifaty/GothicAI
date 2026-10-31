import React, {useEffect, useState} from 'react';
import {cancelRender, continueRender, delayRender, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';
import {CombatData, type Follow, type Hp, type Beat} from '../schema';
import {COLOR, FONT, OUTLINE} from '../theme';
import {sampleHp, selectAction, smoothedTrack, sourceFrame, transformPoint, type Viewport} from './followMath';

const recordings = new Map<string, Promise<CombatData>>();

const loadRecording = (path: string) => {
  let pending = recordings.get(path);
  if (!pending) {
    pending = fetch(staticFile(path)).then(async (response) => {
      if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`);
      return CombatData.parse(await response.json());
    });
    recordings.set(path, pending);
  }
  return pending;
};

// Hold the render until telemetry is ready, including when seeking directly to a still.
const Recording: React.FC<{path: string; children: (data: CombatData) => React.ReactNode}> = ({path, children}) => {
  const [data, setData] = useState<CombatData>();
  const [handle] = useState(() => delayRender(`Combat data: ${path}`));
  useEffect(() => {
    let active = true;
    loadRecording(path).then((recording) => {
      if (active) setData(recording);
      continueRender(handle);
    }).catch((error) => { if (active) cancelRender(error); });
    return () => { active = false; continueRender(handle); };
  }, [path, handle]);
  return data ? children(data) : null;
};

const FADE = 0.1;

const ActionBadge: React.FC<{
  spec: Follow; data: CombatData; frame: number; viewport: Viewport; zoom: Beat['zoom']; t: number;
}> = ({spec, data, frame, viewport, zoom, t}) => {
  const action = selectAction(data.actions, spec.names, frame, data.fps, spec.hold);
  if (!action || t < spec.at || (spec.until !== undefined && t >= spec.until)) return null;

  let point = smoothedTrack(data.track, frame, data.fps);
  let opacity = 1;
  // A short fade at the last known position on loss of tracking. Never interpolate across a null.
  if (!point) {
    let last: CombatData['track'][number] | undefined;
    for (let i = data.track.length - 1; i >= 0; i--) {
      if (data.track[i][0] <= frame && data.track[i][1] !== null) {
        last = data.track[i];
        break;
      }
    }
    if (!last) return null;
    opacity = Math.max(0, 1 - (frame - last[0]) / (FADE * data.fps));
    if (opacity <= 0) return null;
    point = smoothedTrack(data.track, last[0], data.fps);
  }
  if (!point) return null;
  const pin = transformPoint(point, data.size, viewport, zoom, t);
  const x = pin.x - viewport.left;
  const y = pin.y - viewport.top;
  if (x < 0 || x > viewport.width || y < 0 || y > viewport.height) return null;

  const previous = selectAction(data.actions, spec.names, action.frame - 1, data.fps, spec.hold);
  const changing = previous && previous.name !== action.name && frame < previous.frame + spec.hold * data.fps;
  const mix = changing ? Math.min(1, (frame - action.frame + 1) / (FADE * data.fps)) : 1;
  const alignment = x < 170 ? '0%' : x > viewport.width - 170 ? '-100%' : '-50%';
  const badge = (name: string, alpha: number, key: string) => (
    <div key={key} style={{gridArea: '1 / 1', opacity: alpha, padding: '9px 18px', textAlign: 'center'}}>{name}</div>
  );
  return (
    <div style={{position: 'absolute', left: x, top: y + spec.offsetY, opacity,
      transform: `translate(${alignment}, -100%)`, display: 'grid', whiteSpace: 'nowrap',
      background: 'rgba(14,11,6,0.88)', border: `2px solid ${COLOR.now}`, borderRadius: 12,
      color: COLOR.now, fontFamily: FONT.body, fontWeight: 800, fontSize: 34, textShadow: OUTLINE}}>
      {changing && mix < 1 && badge(previous.name, 1 - mix, 'previous')}
      {badge(action.name, mix, 'current')}
    </div>
  );
};

const HpBars: React.FC<{spec: Hp; data: CombatData; frame: number; t: number; insetTop: number}> = ({spec, data, frame, t, insetTop}) => {
  const values = sampleHp(data.hp, frame);
  if (t < spec.at || !values) return null;
  return (
    <div style={{position: 'absolute', left: 32, top: insetTop, width: 300,
      padding: '13px 18px', borderRadius: 12, background: 'rgba(14,11,6,0.84)',
      fontFamily: FONT.body, color: COLOR.text, fontSize: 22, fontWeight: 700}}>
      {values.map((value, i) => {
        const color = i === 0 ? COLOR.now : COLOR.skill;
        return (
          <div key={i} style={{marginTop: i ? 12 : 0}}>
            <div style={{display: 'flex', justifyContent: 'space-between', gap: 12, marginBottom: 5}}>
              <span style={{color}}>{spec.labels[i]}</span>
              <span style={{fontVariantNumeric: 'tabular-nums'}}>{Math.round(value)} / 300</span>
            </div>
            <div style={{height: 12, borderRadius: 4, background: 'rgba(255,255,255,0.18)', overflow: 'hidden'}}>
              <div style={{height: '100%', width: `${Math.max(0, Math.min(1, value / 300)) * 100}%`, background: color}} />
            </div>
          </div>
        );
      })}
    </div>
  );
};

export const CombatOverlay: React.FC<{
  follow?: Follow; hp?: Hp; from: number; beat: Beat; viewport: Viewport; insetTop: number;
}> = ({follow, hp, from, beat, viewport, insetTop}) => {
  const {fps} = useVideoConfig();
  const t = useCurrentFrame() / fps;
  // Quantize trim/freeze exactly like Picture, then convert to the recording's frame rate.
  const seconds = sourceFrame(from, t, fps, beat.freeze) / fps;
  return (
    <div style={{position: 'absolute', left: viewport.left, top: viewport.top,
      width: viewport.width, height: viewport.height, overflow: 'hidden', pointerEvents: 'none'}}>
      {follow && <Recording key={`follow-${follow.data}`} path={follow.data}>
        {(data) => <ActionBadge spec={follow} data={data}
          frame={Math.round(seconds * data.fps)} viewport={viewport} zoom={beat.zoom} t={t} />}
      </Recording>}
      {hp && <Recording key={`hp-${hp.data}`} path={hp.data}>
        {(data) => <HpBars spec={hp} data={data}
          frame={Math.round(seconds * data.fps)} t={t} insetTop={insetTop} />}
      </Recording>}
    </div>
  );
};
