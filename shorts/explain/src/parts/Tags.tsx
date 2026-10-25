import React from 'react';
import {AbsoluteFill, random, useCurrentFrame, useVideoConfig} from 'remotion';
import type {TagT} from '../schema';
import {chance} from '../dice';
import {COLOR, FONT, OUTLINE} from '../theme';
import {rise, shown} from './anim';
import {enterTransform} from './motion';

// `{chance:exe:40:14.7}`: the chance by that rule for that talent and distance, as the game counts it
const withChances = (text: string) =>
  text.replace(/\{chance:(exe|opengothic):(\d+):([\d.]+)\}/g, (_, rule, talent, d) =>
    String(Math.round(chance(rule, Number(talent), Number(d)))),
  );

const SHIFT = {center: 'translateX(-50%)', right: 'translateX(-36px)', left: 'translateX(calc(-100% + 36px))'};

// a label over a thing in the picture: a pin at (x, y), the box above it (opening to the side near an edge), its
// lines appear one by one; a line with `count` runs random numbers (seeded: the same every render) and stops on
// the real one
export const Tags: React.FC<{tags: TagT[]}> = ({tags}) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const t = frame / fps;
  return (
    <AbsoluteFill>
      {tags.map((tag, i) => {
        const o = shown(t, tag.at, tag.until);
        if (o <= 0) {
          return null;
        }
        return (
          <div key={i} style={{position: 'absolute', left: tag.x, top: tag.y, opacity: o, transformOrigin: '0 0',
            transform: enterTransform(tag.enter, t, frame, fps, tag.at)}}>
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
              }}
            />
            <div
              style={{
                position: 'absolute',
                bottom: 30,
                left: 0,
                transform: SHIFT[tag.side],
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
                const lo = shown(t, line.at, undefined, 0.12);
                if (lo <= 0) {
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
