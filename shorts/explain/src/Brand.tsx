import React from 'react';
import {AbsoluteFill, OffthreadVideo, staticFile, useVideoConfig} from 'remotion';
import {FPS, H, W} from './theme';

export const STING_FRAMES = Math.round(2.2 * FPS);

export const BrandSting: React.FC<{volume: number}> = ({volume}) => {
  const {width, height} = useVideoConfig();
  const wide = width > height;
  const video = <OffthreadVideo src={staticFile('brand/sting.mp4')} volume={volume}
    style={wide ? {height, width: height * W / H, objectFit: 'contain'} : {width: W, height: H}} />;
  return wide ? <AbsoluteFill style={{background: '#000', alignItems: 'center', justifyContent: 'center'}}>{video}</AbsoluteFill> : video;
};
