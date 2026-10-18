import React from 'react';
import {Img, OffthreadVideo, staticFile} from 'remotion';
import {FPS, H, SEAL, W} from './theme';

export const STING_FRAMES = Math.round(2.2 * FPS);

export const BrandSeal: React.FC = () => (
  <Img
    src={staticFile('brand/seal-corner.png')}
    style={{position: 'absolute', ...SEAL}}
  />
);

export const BrandSting: React.FC<{volume: number}> = ({volume}) => (
  <OffthreadVideo src={staticFile('brand/sting.mp4')} volume={volume} style={{width: W, height: H}} />
);
