import React from 'react';
import {AbsoluteFill, useCurrentFrame, useVideoConfig} from 'remotion';
import type {TextT} from '../schema';
import {COLOR, FONT, OUTLINE, SAFE, W} from '../theme';
import {rise, shown} from './anim';
import {SealSafeText} from './SealSafeText';

// short texts on screen (not captions): a dark band under each, inside the safe area
export const Texts: React.FC<{texts: TextT[]}> = ({texts}) => {
  const t = useCurrentFrame() / useVideoConfig().fps;
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
              top: x.y + rise(t, x.at),
              left: SAFE.left,
              width: W - SAFE.left - SAFE.right,
              display: 'flex',
              justifyContent: 'center',
              opacity: o,
            }}
          >
            <SealSafeText
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
              }}
            >
              {x.text}
            </SealSafeText>
          </div>
        );
      })}
    </AbsoluteFill>
  );
};

// the hook: big, over the first seconds
export const Hook: React.FC<{text: string; until: number}> = ({text, until}) => {
  const t = useCurrentFrame() / useVideoConfig().fps;
  const o = shown(t, 0, until, 0.01, 0.3);
  if (o <= 0) {
    return null;
  }
  return (
    <AbsoluteFill style={{alignItems: 'center', paddingTop: 300, opacity: o}}>
      <div
        style={{
          fontFamily: FONT.title,
          fontWeight: 700,
          fontSize: 104,
          lineHeight: 1.08,
          color: COLOR.text,
          textAlign: 'center',
          width: W - SAFE.left - SAFE.right - 40,
          marginRight: SAFE.right - SAFE.left,
          textShadow: OUTLINE + ', 0 6px 24px rgba(0,0,0,0.8)',
        }}
      >
        {text}
      </div>
    </AbsoluteFill>
  );
};
