"""Extract game speech and convert Audio2Face CSV weights to FaceRig tracks."""
import base64
import json
import struct
import subprocess
import sys
from pathlib import Path

FPS = 30
RATE = 16000
# the 52 skin shapes of the Audio2Face solver (bs_skin_config_Mark.json), the order of its weights
SHAPES = ("eyeBlinkLeft eyeLookDownLeft eyeLookInLeft eyeLookOutLeft eyeLookUpLeft eyeSquintLeft eyeWideLeft "
          "eyeBlinkRight eyeLookDownRight eyeLookInRight eyeLookOutRight eyeLookUpRight eyeSquintRight eyeWideRight "
          "jawForward jawLeft jawRight jawOpen mouthClose mouthFunnel mouthPucker mouthLeft mouthRight mouthSmileLeft "
          "mouthSmileRight mouthFrownLeft mouthFrownRight mouthDimpleLeft mouthDimpleRight mouthStretchLeft "
          "mouthStretchRight mouthRollLower mouthRollUpper mouthShrugLower mouthShrugUpper mouthPressLeft "
          "mouthPressRight mouthLowerDownLeft mouthLowerDownRight mouthUpperUpLeft mouthUpperUpRight browDownLeft "
          "browDownRight browInnerUp browOuterUpLeft browOuterUpRight cheekPuff cheekSquintLeft cheekSquintRight "
          "noseSneerLeft noseSneerRight tongueOut").split()
EMOTIONS = ("amazement", "anger", "cheekiness", "disgust", "fear", "grief", "joy", "outofbreath", "pain", "sadness")


def vdf_files(path):
    """name (upper) -> (offset, size) of the files of a VDF archive (PSVDSC_V2.00)."""
    with open(path, 'rb') as f:
        head = f.read(296)
        count, _files, _time, _size, root, entry = struct.unpack_from('<6I', head, 272)
        f.seek(root)
        table = f.read(count*entry)
    out = {}
    for i in range(count):
        name, offset, size, kind, _attr = struct.unpack_from('<64s4I', table, i*entry)
        if kind & 0x80000000:  # a directory
            continue
        out[name.decode('cp1251').rstrip(' \0').upper()] = (offset, size)
    return out


def wav(gothic, out, names):
    data = Path(gothic)/'Data'
    vdfs = sorted(p for p in data.iterdir() if p.suffix.lower() == '.vdf' and 'speech' in p.name.lower())
    tables = [(p, vdf_files(p)) for p in vdfs]
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    for name in names:
        key = name.upper() + ('' if name.upper().endswith('.WAV') else '.WAV')
        found = [(p, t[key]) for p, t in tables if key in t]
        if not found:
            sys.exit(f'{key}: in no speech VDF of {data}')
        p, (offset, size) = found[-1]
        with open(p, 'rb') as f:
            f.seek(offset)
            raw = f.read(size)
        dst = out/(Path(key).stem + '.wav')
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', '-', '-ac', '1', '-ar', str(RATE), '-c:a', 'pcm_s16le',
                        str(dst)], input=raw, check=True)
        print(f'{dst} <- {p.name}')


def emotion_label(spec):
    """'anger:0.8,disgust:0.3' checked against the names of Audio2Face v3.0."""
    for item in filter(None, spec.split(',')):
        name, _, value = item.partition(':')
        if name not in EMOTIONS or not 0 <= float(value) <= 1:
            sys.exit(f'bad emotion {item!r}: names {", ".join(EMOTIONS)}, strength 0..1')
    return spec


def track(csv, out, emotion=''):
    rows = [[float(v) for v in line.split(',')] for line in Path(csv).read_text().split('\n') if line.strip()]
    if not rows:
        sys.exit(f'{csv}: no frames')
    if any(len(row) < 1 + len(SHAPES) for row in rows):
        sys.exit(f'{csv}: each frame needs a timestamp and {len(SHAPES)} skin weights')
    t = [r[0]/RATE for r in rows]
    n = int(t[-1]*FPS + 1e-6) + 1
    frames = bytearray()
    j = 0
    for i in range(n):
        at = i/FPS
        while j+1 < len(t) and t[j+1] <= at + 1e-9:
            j += 1
        k = min(j+1, len(t)-1)
        a = 0.0 if k == j else min(max((at-t[j])/(t[k]-t[j]), 0.0), 1.0)
        for s in range(len(SHAPES)):
            w = rows[j][1+s]*(1-a) + rows[k][1+s]*a
            frames.append(round(min(max(w, 0.0), 1.0)*255))
    doc = {'fps': FPS, 'shapes': SHAPES, 'data': base64.b64encode(bytes(frames)).decode()}
    if emotion:
        doc['emotion'] = emotion_label(emotion)
    Path(out).write_text(json.dumps(doc, separators=(',', ':')) + '\n')
    print(f'{out}: {n} frames, {n/FPS:.2f} s{", " + emotion if emotion else ""}')
