import {loadFont} from '@remotion/fonts';
import {continueRender, delayRender} from 'remotion';
import {FONT} from './theme';
import montserrat from './fonts/Montserrat.ttf';
import ptSerifBold from './fonts/PTSerif-Bold.ttf';

// shipped with the project (OFL): Google Fonts at render time would bring the Latin subset only
const handle = delayRender('fonts');
export const fontsReady = Promise.all([
  loadFont({family: FONT.title, url: ptSerifBold, weight: '700'}),
  loadFont({family: FONT.body, url: montserrat, weight: '100 900'}),
]).then(() => continueRender(handle));
