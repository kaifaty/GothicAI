import React from 'react';
import {Audio, Sequence, staticFile, useVideoConfig} from 'remotion';
import type {Plan} from '../schema';
import {sounds} from './sfxMap';

// the sound of the motion (parts/sfxMap.ts): each sound starts its `lead` before the frame it belongs to, so its peak
// lands on it. The files are the workspace's sfx/ (a link to ~/.local/share/gothic-ai/sfx/lib, scripts/sfx_lib.py)
export const Sfx: React.FC<{plan: Plan}> = ({plan}) => {
  const {fps} = useVideoConfig();
  return (
    <>
      {sounds(plan).map((s, i) => {
        const from = Math.round(s.start * fps);
        return (
          <Sequence key={i} from={Math.max(0, from)} name={`sfx ${s.kind} ${s.beat}`} layout="none">
            <Audio src={staticFile(s.file)} volume={s.gain} startFrom={Math.max(0, -from)} />
          </Sequence>
        );
      })}
    </>
  );
};
