import {Config} from '@remotion/cli/config';

Config.setEntryPoint('./src/index.ts');
Config.setVideoImageFormat('jpeg');
Config.setOverwriteOutput(true);
// what YouTube asks for: BT.709, limited range (Remotion 4 otherwise writes BT.601, full range)
Config.setColorSpace('bt709');
