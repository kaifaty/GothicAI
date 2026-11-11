# Lip opening (jawOpen + mouthLowerDown) against loudness per clip and model: best correlation, its lag, brow and blink range.
#   python3 sync.py <dir with clips/*.wav and out/<clip>.<mark|v3|v3fix|lam>.csv>
import sys, wave, numpy as np
S=sys.argv[1]
SK="eyeBlinkLeft eyeLookDownLeft eyeLookInLeft eyeLookOutLeft eyeLookUpLeft eyeSquintLeft eyeWideLeft eyeBlinkRight eyeLookDownRight eyeLookInRight eyeLookOutRight eyeLookUpRight eyeSquintRight eyeWideRight jawForward jawLeft jawRight jawOpen mouthClose mouthFunnel mouthPucker mouthLeft mouthRight mouthSmileLeft mouthSmileRight mouthFrownLeft mouthFrownRight mouthDimpleLeft mouthDimpleRight mouthStretchLeft mouthStretchRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper mouthPressLeft mouthPressRight mouthLowerDownLeft mouthLowerDownRight mouthUpperUpLeft mouthUpperUpRight browDownLeft browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft cheekSquintRight noseSneerLeft noseSneerRight tongueOut".split()
ix={n:i for i,n in enumerate(SK)}
for n in "inworld_xardas inworld_hero orig_hero orig_sylvio orig_biff player_mic".split():
    w=wave.open(f'{S}/clips/{n}.wav'); a=np.frombuffer(w.readframes(w.getnframes()),np.int16)/32768
    rms=np.array([np.sqrt((a[i*533:(i+1)*533]**2).mean()+1e-9) for i in range(len(a)//533)])
    env=np.clip(20*np.log10(rms)+50,0,None)
    for m in ['mark','v3','v3fix','lam']:
        d=np.loadtxt(f'{S}/out/{n}.{m}.csv',delimiter=','); t=d[:,0]; W=d[:,1:53]
        k=np.clip(np.searchsorted(t,np.arange(len(env))*533,side='right')-1,0,len(t)-1); W=W[k]
        op=W[:,ix['jawOpen']]+0.5*(W[:,ix['mouthLowerDownLeft']]+W[:,ix['mouthLowerDownRight']])
        best=max(((np.corrcoef(op[max(0,l):len(op)+min(0,l)], env[max(0,-l):len(env)-max(0,l)])[0,1],l) for l in range(-8,9)))
        brow=W[:,[ix['browInnerUp'],ix['browOuterUpLeft'],ix['browOuterUpRight'],ix['browDownLeft'],ix['browDownRight']]].max(1)
        blink=W[:,[ix['eyeBlinkLeft'],ix['eyeBlinkRight']]].max(1)
        print(f"{n:15s} {m:4s} corr {best[0]:.2f} at lag {best[1]*33:+d} ms | open max {op.max():.2f} | brow max {brow.max():.2f} sd {brow.std():.3f} | blink max {blink.max():.2f}")
