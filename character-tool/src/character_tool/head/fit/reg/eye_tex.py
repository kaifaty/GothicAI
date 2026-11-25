# The hero's grey-blue iris from MPFB's CC0 'lightblue' eye texture: the iris (saturated pixels) is pulled 75 % to a
# cool grey of its own brightness and darkened by 15 %; the white and the alpha stay as they are.
#   python eye_tex.py <mpfb data/eyes/materials> <out.png> [iris brightness, 0.85]
import sys, numpy as np
from PIL import Image
D,out=sys.argv[1:3]; K=float(sys.argv[3]) if len(sys.argv)>3 else 0.85
src=np.asarray(Image.open(f'{D}/lightblue_eye.png').convert('RGBA')).astype(float)
im=src[...,:3]; al=src[...,3:]
m=np.clip(((im.max(2)-im.min(2))-15)/30,0,1)[...,None]
grey=(im@[0.299,0.587,0.114])[...,None]*np.array([0.96,1.0,1.05])
o=im*(1-m)+m*(0.25*im+0.75*grey)*K
Image.fromarray(np.clip(np.concatenate([o,al],2),0,255).astype(np.uint8),'RGBA').save(out)
