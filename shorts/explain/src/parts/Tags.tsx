import React from 'react';
import {AbsoluteFill, random, useCurrentFrame, useVideoConfig} from 'remotion';
import type {TagT} from '../schema';
import {COLOR, FONT, OUTLINE} from '../theme';
import {rise, shown} from './anim';
import {withChances} from './carryMath';
import {enterTransform, MOTION} from './motion';

const SHIFT = {center: 'translateX(-50%)', right: 'translateX(-36px)', left: 'translateX(calc(-100% + 36px))'};
// the point of the box over the pin, where it grows from
const ORIGIN = {center: '50% 100%', right: '36px 100%', left: 'calc(100% - 36px) 100%'};

// a label over a thing in the picture: a pin at (x, y), the box above it (opening to the side near an edge), its
// lines appear one by one; a line with `count` runs random numbers (seeded: the same every render) and stops on
// the real one. The pin lands first and the box follows from it `MOTION.follow` later, so the label reads as
// coming out of the thing it marks. A line the next beat carries off (`carried`) leaves its box at the cut
export const Tags: React.FC<{tags: TagT[]; carried?: {tag: number; line: number; at: number}}> = ({tags, carried}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const t = frame / fps;
  return (
    <AbsoluteFill>
      {tags.map((tag, i) => {
        const o = shown(t, tag.at, tag.until);
        // a one-line tag whose line flew off takes its pin and box with it: no empty box under a transition
        const emptied = carried?.tag === i && tag.lines.length === 1 && t >= carried.at;
        if (o <= 0 || emptied) {
          return null;
        }
        const boxAt = tag.at + MOTION.follow;
        return (
          <div key={i} style={{position: 'absolute', left: tag.x, top: tag.y, opacity: o}}>
            <div
              style={{
                position: 'absolute',
                left: -9,
                top: -9,
                width: 18,
                height: 18,
                borderRadius: 9,
                border: `3px solid ${COLOR.text}`,
                boxShadow: '0 0 8px #000',
                transform: enterTransform(tag.enter, t, frame, fps, tag.at),
              }}
            />
            <div
              style={{
                position: 'absolute',
                bottom: 30,
                left: 0,
                opacity: shown(t, boxAt),
                transformOrigin: ORIGIN[tag.side],
                transform: `${SHIFT[tag.side]} ${enterTransform(tag.enter, t, frame, fps, boxAt)}`,
                background: COLOR.shade,
                borderRadius: 16,
                padding: '12px 22px',
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                gap: 2,
                whiteSpace: 'nowrap',
              }}
            >
              {tag.lines.map((line, j) => {
                const gone = carried && carried.tag === i && carried.line === j && t >= carried.at;
                const lo = gone ? 0 : shown(t, line.at, undefined, 0.12);
                if (lo <= 0 && !gone) {
                  return null;
                }
                let text = withChances(line.text);
                if (line.count) {
                  const settled = t >= line.at + line.count.dur;
                  const n = settled ? line.count.to : Math.floor(random(`tag-${i}-${j}-${frame}`) * 100);
                  text = text.replace('{n}', String(n));
                }
                return (
                  <div
                    key={j}
                    style={{
                      opacity: lo,
                      transform: `translateY(${rise(t, line.at, 10)}px)`,
                      fontFamily: FONT.body,
                      fontWeight: 800,
                      fontSize: 52,
                      color: line.color ?? COLOR.text,
                      textShadow: OUTLINE,
                      fontVariantNumeric: 'tabular-nums',
                    }}
                  >
                    {text}
                  </div>
                );
              })}
            </div>
          </div>
        );
      })}
    </AbsoluteFill>
  );
};
