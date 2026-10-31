import React from 'react';
import {Composition, type CalculateMetadataFunction} from 'remotion';
import {STING_FRAMES} from './Brand';
import {Explainer} from './Explainer';
import {fontsReady} from './fonts';
import {Plan} from './schema';
import {FPS, FRAME, H, W} from './theme';

// the dice chart demo (use --props '{"brand":false}' without assets; real plans come from the workspace)
const demo: Plan = Plan.parse({
  lang: 'ru',
  hook: {text: 'Шаг за 15 м — минус треть шанса', until: 2},
  beats: [
    {
      id: 'chart',
      kind: 'card',
      dur: 8,
      chart: {
        type: 'dice',
        talent: 40,
        axisX: 'метры',
        axisY: 'шанс, %',
        source: 'формула Gothic2.exe, навык 40%',
        curves: [{curve: 'exe', name: 'Gothic2.exe', draw: [[0.3, 0], [3.3, 15], [4.6, 15], [6.2, 45]]}],
        talentMark: {at: 3.0, text: 'навык 40%'},
        stepMark: {at: 4.0, text: '−⅓'},
        markers: [{d: 16, curve: 'exe', at: 6.4, text: '16 м: {v}%'}],
      },
    },
  ],
});

// the plan is checked here: a missing or misspelt field stops the render with its path
const metadata: CalculateMetadataFunction<Plan> = async ({props}) => {
  await fontsReady;
  const plan = Plan.parse(props);
  const dur = plan.beats.reduce((s, b) => s + b.dur, 0);
  return {props: plan, ...FRAME[plan.format], durationInFrames: Math.round(dur * FPS) + (plan.brand ? STING_FRAMES : 0)};
};

export const Root: React.FC = () => (
  <Composition
    id="Explainer"
    component={Explainer}
    width={W}
    height={H}
    fps={FPS}
    durationInFrames={1}
    defaultProps={demo}
    calculateMetadata={metadata}
  />
);
