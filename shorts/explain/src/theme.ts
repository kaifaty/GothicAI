// The look shared by the explainer series: one palette, two fonts.

export const W = 1080;
export const H = 1920;
export const FPS = 30;

// the Shorts interface covers these: no text under them
export const SAFE = {top: 150, bottom: 380, right: 150, left: 60};

export const FRAME = {
  tall: {width: W, height: H},
  wide: {width: 1920, height: 1080},
};

// a tall card's content is drawn this much higher than its own coordinates, so its lowest labels clear the captions
// (top 1170) by about 100 px
export const CARD_LIFT = 80;
// a tall card's source line ends over the bottom 380 and grows upwards. The slow push of a held card (up to 5 %
// about the safe area's centre, parts/timing.ts) carries its foot 35 px down, so it ends that much higher; two lines
// start at about 1440, still clear of three lines of captions (they end at about 1425)
export const SOURCE_BOTTOM = SAFE.bottom + 36;

// Wide videos have ordinary 5% margins, clear of the frame rather than the Shorts UI.
export const WIDE_SAFE = {top: 54, bottom: 54, left: 96, right: 96};

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
