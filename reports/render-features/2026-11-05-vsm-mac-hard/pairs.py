"""Side-by-side pairs of two bench runs for the review, a label over each half.

    uv run --with pillow --with numpy python -I pairs.py <out dir> <name> <left label>=<run dir> <right label>=<run dir>
        [--clips a,b,c] [--shots x,y,z] [--size 1920x1080]

Clips (<run>/<clip>.mp4) go to <out>/<name>-<clip>.mp4 (ffmpeg hstack, H.264 CRF 16), shots (<run>/<shot>.png) to
<out>/<name>-<shot>.png. --size scales each half with Lanczos first: the bench captures the internal frame (75 %:
1440x810 in a 960x540 window), before MetalFX Spatial (README.md, «Клипы и кадры»).
"""
import argparse
import pathlib
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont

FONT = '/System/Library/Fonts/Supplemental/Arial Bold.ttf'


def label_layer(w: int, h: int, left: str, right: str) -> Image.Image:
    img = Image.new('RGBA', (2*w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, max(18, h//27))
    for x0, text in ((0, left), (w, right)):
        box = d.textbbox((0, 0), text, font=font)
        tw, th = box[2]-box[0], box[3]-box[1]
        x = x0 + (w - tw)//2
        pad = th//2
        d.rectangle((x - 2*pad, 0, x + tw + 2*pad, th + 3*pad), fill=(0, 0, 0, 150))
        d.text((x - box[0], pad - box[1] + pad//2), text, font=font, fill=(255, 255, 255, 255))
    # the seam between the halves
    d.line((w - 1, 0, w - 1, h), fill=(0, 0, 0, 255), width=2)
    return img


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('name')
    ap.add_argument('left')
    ap.add_argument('right')
    ap.add_argument('--clips', default='')
    ap.add_argument('--shots', default='')
    ap.add_argument('--size', default='', help='WxH of each half: Lanczos from the captured frame')
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (ll, ld), (rl, rd) = (s.split('=', 1) for s in (a.left, a.right))
    ld, rd = pathlib.Path(ld), pathlib.Path(rd)
    for shot in filter(None, a.shots.split(',')):
        A = Image.open(ld/f'{shot}.png').convert('RGB')
        B = Image.open(rd/f'{shot}.png').convert('RGB')
        assert A.size == B.size, (shot, A.size, B.size)
        if a.size:
            size = tuple(map(int, a.size.split('x')))
            A, B = A.resize(size, Image.LANCZOS), B.resize(size, Image.LANCZOS)
        w, h = A.size
        img = Image.new('RGB', (2*w, h))
        img.paste(A, (0, 0))
        img.paste(B, (w, 0))
        img.paste(lbl := label_layer(w, h, ll, rl), (0, 0), lbl)
        dst = out/f'{a.name}-{shot}.png'
        img.save(dst)
        print(dst)
    for clip in filter(None, a.clips.split(',')):
        probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                                'stream=width,height', '-of', 'csv=p=0', str(ld/f'{clip}.mp4')],
                               capture_output=True, text=True, check=True).stdout.strip()
        w, h = map(int, probe.split(','))
        scale = ''
        if a.size:
            w, h = map(int, a.size.split('x'))
            scale = f'scale={w}:{h}:flags=lanczos,'
        with tempfile.TemporaryDirectory() as tmp:
            lp = pathlib.Path(tmp)/'label.png'
            label_layer(w, h, ll, rl).save(lp)
            dst = out/f'{a.name}-{clip}.mp4'
            subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', str(ld/f'{clip}.mp4'), '-i', str(rd/f'{clip}.mp4'),
                            '-i', str(lp), '-filter_complex', f'[0:v]{scale}setsar=1[a];[1:v]{scale}setsar=1[b];[a][b]hstack=inputs=2:shortest=1[s];[s][2:v]overlay=0:0',
                            '-c:v', 'libx264', '-preset', 'slow', '-crf', '16', '-pix_fmt', 'yuv420p', '-an',
                            '-movflags', '+faststart', str(dst)], check=True)
            print(dst)


if __name__ == '__main__':
    main()
