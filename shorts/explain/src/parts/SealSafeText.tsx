import React, {createContext, useContext, useEffect, useLayoutEffect, useRef, useState} from 'react';
import {AbsoluteFill, continueRender, delayRender, useCurrentFrame} from 'remotion';
import {SEAL, SEAL_TEXT_GAP, W} from '../theme';

const SealArea = createContext<{enabled: boolean; root: React.RefObject<HTMLDivElement | null>} | null>(null);

export const SealSafeArea: React.FC<React.PropsWithChildren<{enabled: boolean}>> = ({enabled, children}) => {
  const root = useRef<HTMLDivElement>(null);
  return (
    <AbsoluteFill ref={root} style={{background: '#000'}}>
      <SealArea.Provider value={{enabled, root}}>{children}</SealArea.Provider>
    </AbsoluteFill>
  );
};

// Measure the actual label, rather than its full-width alignment container. Short labels keep their
// position; only a block touching the seal moves below it. Captions, hooks and cards already start lower.
export const SealSafeText: React.FC<React.HTMLAttributes<HTMLDivElement>> = ({style, children, ...props}) => {
  const area = useContext(SealArea);
  const ref = useRef<HTMLDivElement>(null);
  const [shift, setShift] = useState(0);
  const [fontsReady, setFontsReady] = useState(false);
  const [handle] = useState(() => delayRender('text clear of the brand seal'));
  useCurrentFrame(); // animated labels must be measured again on each frame

  useEffect(() => {
    let mounted = true;
    document.fonts.ready.then(() => {
      if (mounted) setFontsReady(true);
    });
    return () => {
      mounted = false;
      continueRender(handle);
    };
  }, [handle]);

  useLayoutEffect(() => {
    if (!area?.enabled) {
      if (shift !== 0) setShift(0);
      continueRender(handle);
      return;
    }
    if (!fontsReady || !ref.current || !area.root.current) return;
    const root = area.root.current.getBoundingClientRect();
    const box = ref.current.getBoundingClientRect();
    const scale = root.width / W;
    if (scale <= 0) return;
    const left = (box.left - root.left) / scale;
    const right = (box.right - root.left) / scale;
    const top = (box.top - root.top) / scale - shift;
    const bottom = (box.bottom - root.top) / scale - shift;
    const overlaps =
      left < SEAL.left + SEAL.width && right > SEAL.left &&
      top < SEAL.top + SEAL.height && bottom > SEAL.top;
    const next = overlaps ? SEAL.top + SEAL.height + SEAL_TEXT_GAP - top : 0;
    if (Math.abs(next - shift) > 0.01) {
      setShift(next);
    } else {
      continueRender(handle);
    }
  });

  return (
    <div ref={ref} {...props} style={{...style, transform: `${style?.transform ?? ''} translateY(${shift}px)`}}>
      {children}
    </div>
  );
};
