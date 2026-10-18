// The look shared by the explainer series: one palette, two fonts, the Shorts safe area.

export const W = 1080;
export const H = 1920;
export const FPS = 30;

// the Shorts interface covers these: no text under them
export const SAFE = {top: 150, bottom: 380, right: 150, left: 60};
export const SEAL = {left: W - 220, top: 110, width: 110, height: 110};
export const SEAL_TEXT_GAP = 16; // leave room for outlines and shadows after moving a label

export const COLOR = {
  before: '#D8D8D8', // the old rule (the dice, the auto aim)
  now: '#FFD678', // the new rule; the gold of shorts/compare
  skill: '#7EC8FF',
  strength: '#FF7A66',
  dexterity: '#8BE08B',
  miss: '#FF5A5A',
  text: '#FFFFFF',
  shade: 'rgba(0,0,0,0.62)',
  card: '#14110D',
  grid: 'rgba(255,255,255,0.14)',
  faint: '#4d463b',
};

export const FONT = {
  title: 'PT Serif', // hooks and titles
  body: 'Montserrat', // labels, numbers (tabular figures), unbranded captions
};

export const OUTLINE = '0 0 6px rgba(0,0,0,0.9), 0 0 2px #000, 0 0 2px #000, 0 0 2px #000';
