import React from 'react';
import {AbsoluteFill, useCurrentFrame, useVideoConfig} from 'remotion';
import {measureText} from '@remotion/layout-utils';
import type {Beat} from '../schema';
import {COLOR, FONT, OUTLINE} from '../theme';
import {shown} from './anim';
import {arcPoint, distance, settledText, TAG, tagLineCentre, type Point} from './carryMath';
import {EASE, progress} from './motion';
import {travelDur} from './timing';

const measure = (text: string) =>
  measureText({text, fontFamily: FONT.body, fontSize: TAG.font, fontWeight: '800', fontVariantNumeric: 'tabular-nums'});

// where a beat's carry starts, what it reads and its colour: a tag line of the beat before as Tags.tsx leaves it on
// the cut, or the plan's point and text
export const carryStart = (beat: Beat, prev: Beat): {from: Point; text: string; color?: string} | null => {
  const c = beat.carry;
  if (!c) return null;
  if (Array.isArray(c.from)) return {from: {x: c.from[0], y: c.from[1]}, text: c.text ?? '', color: c.color};
  const tag = prev.tags[c.from.tag];
  const line = tag.lines[c.from.line];
  return {
    from: tagLineCentre(tag, c.from.line, prev.dur, measure),
    text: c.text ?? settledText(line),
    color: c.color ?? line.color,
  };
};

// one thing carried across the cut, so the two beats share it instead of a slideshow's swap: it takes over the tag
// line's place on the cut frame, waits there until `at`, flies along an arc (in-out, a move between two rests)
// to `to`, growing to `size`. Its dark pill fades out on the way onto a card, which has a dark ground of its own
export const Carry: React.FC<{beat: Beat; prev: Beat}> = ({beat, prev}) => {
  const {fps} = useVideoConfig();
  const t = useCurrentFrame() / fps;
  const c = beat.carry;
  const start = carryStart(beat, prev);
  if (!c || !start) return null;
  const to = {x: c.to[0], y: c.to[1]};
  const dur = c.dur ?? travelDur(distance(start.from, to));
  const o = c.until === undefined ? 1 : shown(t, 0, c.until, 0);
  if (o <= 0) return null;
  const p = progress(t, c.at, dur, EASE.inOut);
  const at = arcPoint(start.from, to, c.arc, p);
  const scale = 1 + ((c.size ?? TAG.font) / TAG.font - 1) * p;
  const pill = beat.kind === 'card' ? 1 - p : 1;
  return (
    <AbsoluteFill style={{pointerEvents: 'none'}}>
      <div
        style={{
          position: 'absolute',
          left: at.x,
          top: at.y,
          opacity: o,
          transform: `translate(-50%, -50%) scale(${scale})`,
          padding: `${TAG.padY}px ${TAG.padX}px`,
          borderRadius: 16,
          background: `rgba(0,0,0,${0.62 * pill})`,
          whiteSpace: 'nowrap',
          fontFamily: FONT.body,
          fontWeight: 800,
          fontSize: TAG.font,
          color: start.color ?? COLOR.text,
          textShadow: OUTLINE,
          fontVariantNumeric: 'tabular-nums',
        }}
      >
        {start.text}
      </div>
    </AbsoluteFill>
  );
};
