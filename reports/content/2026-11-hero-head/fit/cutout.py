"""Cut the flat concept background out of a view: background = pixels near the background colour that are
connected to the image border (flood fill), so grey eyes, shadows and the shirt inside the figure stay opaque.
    python3 cutout.py IN.png OUT.png  X Y SIZE  [OUT_SIZE]   (square crop at X,Y of SIZE px, then resize)"""
import subprocess, sys, numpy as np
src,dst,x,y,s=sys.argv[1],sys.argv[2],*map(int,sys.argv[3:6]); out=int(sys.argv[6]) if len(sys.argv)>6 else 1024
raw=subprocess.run(['ffmpeg','-loglevel','error','-i',src,'-vf',f'crop={s}:{s}:{x}:{y}','-f','rawvideo','-pix_fmt','rgb24','-'],capture_output=True,check=True).stdout
im=np.frombuffer(raw,np.uint8).reshape(s,s,3).astype(np.float32)
bg=np.median(np.concatenate([im[0],im[-1],im[:,0],im[:,-1]]),axis=0)
d=np.abs(im-bg).max(axis=2)
cand=d<14
m=np.zeros_like(cand); m[0]=cand[0]; m[-1]=cand[-1]; m[:,0]=cand[:,0]; m[:,-1]=cand[:,-1]
while True:
    g=m.copy(); g[1:]|=m[:-1]; g[:-1]|=m[1:]; g[:,1:]|=m[:,:-1]; g[:,:-1]|=m[:,1:]; g&=cand
    if (g==m).all(): break
    m=g
# soft edge: alpha ramps over the colour distance at the background border
a=np.where(m,0.0,1.0)
edge=m & ~np.pad(m,1,constant_values=True)[2:,1:-1] | m & ~np.pad(m,1,constant_values=True)[:-2,1:-1]
a[edge]=np.clip(d[edge]/14.0,0,1)
rgba=np.dstack([im,a*255]).clip(0,255).astype(np.uint8)
subprocess.run(['ffmpeg','-loglevel','error','-y','-f','rawvideo','-pix_fmt','rgba','-s',f'{s}x{s}','-i','-','-vf',f'scale={out}:{out}:flags=lanczos','-pix_fmt','rgba',dst],input=rgba.tobytes(),check=True)
print('bg',bg.round(1),'opaque %.1f%%'%(100*a.mean()))
