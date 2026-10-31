import React from 'react';
import {AbsoluteFill, useCurrentFrame, useVideoConfig} from 'remotion';
import type {TextT} from '../schema';
import {COLOR, FONT, OUTLINE, SAFE, WIDE_SAFE} from '../theme';
import {rise, shown} from './anim';
import {enterTransform, MOTION} from './motion';

// short texts on screen (not captions): a dark band under each, inside the safe area. The band arrives first and
// its words settle into it `MOTION.follow` later
export const Texts: React.FC<{texts: TextT[]}> = ({texts}) => {
  const {fps, width, height} = useVideoConfig();
  const wide = width > height;
  const safe = wide ? WIDE_SAFE : SAFE;
  const frame = useCurrentFrame();
  const t = frame / fps;
  return (
    <AbsoluteFill>
      {texts.map((x, i) => {
        const o = shown(t, x.at, x.until);
        if (o <= 0) {
          return null;
        }
        return (
          <div
            key={i}
            style={{
              position: 'absolute',
              top: x.y ?? (wide ? 140 : 260),
              transform: enterTransform(x.enter, t, frame, fps, x.at),
              left: safe.left,
              width: width - safe.left - safe.right,
              display: 'flex',
              justifyContent: 'center',
              opacity: o,
            }}
          >
            <span
              style={{
                fontFamily: FONT.body,
                fontWeight: 800,
                fontSize: x.size,
                lineHeight: 1.15,
                color: x.color ?? COLOR.text,
                background: COLOR.shade,
                padding: '10px 22px',
                borderRadius: 14,
                textAlign: 'center',
                textShadow: OUTLINE,
                fontVariantNumeric: 'tabular-nums',
                ...(wide ? {maxWidth: '100%', boxSizing: 'border-box', overflowWrap: 'anywhere'} as const : {}),
              }}
            >
              <span style={{display: 'inline-block', opacity: shown(t, x.at + MOTION.follow),
                transform: `translateY(${rise(t, x.at + MOTION.follow, 6)}px)`}}>
                {x.text}
              </span>
            </span>
          </div>
        );
      })}
    </AbsoluteFill>
  );
};

// the hook: big, over the first seconds
export const Hook: React.FC<{text: string; until: number}> = ({text, until}) => {
  const {fps, width, height} = useVideoConfig();
  const wide = width > height;
  const safe = wide ? WIDE_SAFE : SAFE;
  const t = useCurrentFrame() / fps;
  const o = shown(t, 0, until, 0.01, 0.3);
  if (o <= 0) {
    return null;
  }
  return (
    <AbsoluteFill style={{alignItems: 'center', paddingTop: wide ? safe.top : 300, opacity: o}}>
      <div
        style={{
          fontFamily: FONT.title,
          fontWeight: 700,
          fontSize: wide ? 80 : 104,
          lineHeight: 1.08,
          color: COLOR.text,
          textAlign: 'center',
          width: width - safe.left - safe.right - (wide ? 0 : 40),
          marginRight: safe.right - safe.left,
          ...(wide ? {overflowWrap: 'anywhere'} as const : {}),
          textShadow: OUTLINE + ', 0 6px 24px rgba(0,0,0,0.8)',
        }}
      >
        {text}
      </div>
    </AbsoluteFill>
  );
};
