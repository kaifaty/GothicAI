# zoom.py IMG OUT x0 y0 w h k [pts.json view]: crop scaled k times with a 10 px grid labelled in frame pixels
import sys, json
from PIL import Image, ImageDraw
im=Image.open(sys.argv[1]).convert('RGB'); x0,y0,w,h,k=map(int,sys.argv[3:8])
c=im.crop((x0,y0,x0+w,y0+h)).resize((w*k,h*k),Image.LANCZOS); d=ImageDraw.Draw(c)
for x in range((x0//10+1)*10,x0+w,10):
    d.line([((x-x0)*k,0),((x-x0)*k,h*k)],fill=(255,255,0) if x%50==0 else (90,90,90),width=1)
    if x%50==0: d.text(((x-x0)*k+2,2),str(x),fill=(255,255,0))
for y in range((y0//10+1)*10,y0+h,10):
    d.line([(0,(y-y0)*k),(w*k,(y-y0)*k)],fill=(255,255,0) if y%50==0 else (90,90,90),width=1)
    if y%50==0: d.text((2,(y-y0)*k+2),str(y),fill=(255,255,0))
if len(sys.argv)>8:
    for n,(x,y) in json.load(open(sys.argv[8]))[sys.argv[9]].items():
        X,Y=(x-x0)*k,(y-y0)*k
        if 0<=X<w*k and 0<=Y<h*k: d.ellipse([X-4,Y-4,X+4,Y+4],outline=(255,0,0),width=2); d.text((X+6,Y-12),n,fill=(255,60,60))
c.save(sys.argv[2])
