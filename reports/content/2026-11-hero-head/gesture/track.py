#!/usr/bin/env python3
"""How the head moves on the screen in a recording of scripts/bench/gesture-head-clip.bench (gesture-head.md).

  track.py <bench out dir> <take> [<take> ...]

The camera of the takes (`cam face 38`) is put from the head bone every frame, 38 cm in front of it: a move of the
whole head moves the wall behind, a turn, a bend or a tilt of the head moves the face. Per frame the median optical
flow (pyramidal Lucas-Kanade, frame to frame, checked back) of the points in two regions of the 1280x720 frame: the
upper face (brows, eyes, nose bridge) and the wall left and right of the head. Writes <dir>/track-<take>.csv (frame,
face_dx, face_dy, wall_dx, wall_dy; px) and prints the largest and the 95th percentile of the face against the wall,
px and degrees of view (vertical view 67.5 degrees: 0.106 degrees a pixel at the centre). The eyes are 11 cm above the
bone and 8 cm in front of it, 30 cm from the camera: a tilt of 1 degree moves them 3.4 px, a turn or a bend 2.5 px.
"""
import csv
import sys
from pathlib import Path

import cv2
import numpy as np

F = 360/np.tan(np.radians(67.5/2))   # px
DEG = np.degrees(1/F)
FACE = (540, 740, 170, 300)
WALL = [(20, 440, 40, 330), (860, 1260, 40, 330)]


def points(gray, boxes):
    mask = np.zeros_like(gray)
    for x0, x1, y0, y1 in boxes:
        mask[y0:y1, x0:x1] = 255
    p = cv2.goodFeaturesToTrack(gray, 200, 0.01, 6, mask=mask)
    return p if p is not None else np.zeros((0, 1, 2), np.float32)


def flow(prev, cur, pts):
    if len(pts) == 0:
        return np.array([np.nan, np.nan])
    nxt, st, _ = cv2.calcOpticalFlowPyrLK(prev, cur, pts, None, winSize=(21, 21), maxLevel=3)
    back, st2, _ = cv2.calcOpticalFlowPyrLK(cur, prev, nxt, None, winSize=(21, 21), maxLevel=3)
    ok = (st[:, 0] == 1) & (st2[:, 0] == 1) & (np.linalg.norm((back-pts)[:, 0], axis=1) < 0.5)
    if ok.sum() < 5:
        return np.array([np.nan, np.nan])
    return np.median((nxt-pts)[ok, 0], axis=0)


def track(video):
    cap = cv2.VideoCapture(str(video))
    ok, frame = cap.read()
    if not ok:
        sys.exit(f'no frames in {video}')
    prev = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    rows = [(0, 0, 0, 0, 0)]
    n = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        n += 1
        cur = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        rows.append((n, *flow(prev, cur, points(prev, [FACE])), *flow(prev, cur, points(prev, WALL))))
        prev = cur
    return np.array(rows, float)


def read(path):
    """The face against the wall of each frame, px: (dx, dy), and the wall's own move."""
    rows = list(csv.DictReader(open(path)))[1:]
    f = np.array([[float(r['face_dx'])-float(r['wall_dx']), float(r['face_dy'])-float(r['wall_dy'])] for r in rows])
    w = np.hypot([float(r['wall_dx']) for r in rows], [float(r['wall_dy']) for r in rows])
    return f, w


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    src = Path(argv[0])
    for take in argv[1:]:
        r = track(src/f'{take}.mp4')
        out = src/f'track-{take}.csv'
        with open(out, 'w') as f:
            w = csv.writer(f)
            w.writerow(['frame', 'face_dx', 'face_dy', 'wall_dx', 'wall_dy'])
            for row in r:
                w.writerow([int(row[0])]+[f'{v:.3f}' for v in row[1:]])
        face, wall = read(out)
        v = np.hypot(face[:, 0], face[:, 1])
        v = v[~np.isnan(v)]
        print(f'{take}: the face against the wall, px a frame: max {v.max():.1f} ({v.max()*DEG:.2f} deg), '
              f'p95 {np.percentile(v, 95):.1f}; the wall max {np.nanmax(wall):.1f}')


if __name__ == '__main__':
    main(sys.argv[1:])
