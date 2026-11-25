#!/usr/bin/env python3
"""Capture and compare the two arms of the isolated tree Nanite content route."""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import time
from pathlib import Path

import numpy as np
from fir_bough_pops import scores, standout
from impostor_bake import dds_mips

WIDTH, HEIGHT, FPS = 1280, 720, 24
SAMPLE_WIDTH, SAMPLE_HEIGHT = WIDTH // 8, HEIGHT // 8
PROJECTION = 1.4966058

# Gothic centimetres, up Y. Every shot uses the same camera and pose in both arms.
SHOTS = (
    ('gust', 3, (-2900, 700, -200), (-2900, 700, -200), (0, 900, 0), 38),
    ('close', 3, (-1470, 440, -880), (-820, 430, -1260), (0, 700, 0), 38),
    ('upper', 2, (-2050, 2380, -920), (-900, 2670, -1560), (0, 1050, 0), 38),
    ('far', 5, (-2460, 910, -1210), (-19000, 1920, -9300), (0, 900, 0), 38),
    ('low-sun', 3, (-2930, 860, -2480), (-2160, 720, -3170), (420, 570, 280), 8),
)


def route(threshold: float):
    index = 0
    for name, seconds, a, b, target, elevation in SHOTS:
        angle = math.radians(elevation)
        sun = (-.85 * math.cos(angle), math.sin(angle), -math.sqrt(1 - .85 ** 2) * math.cos(angle))
        for frame in range(seconds * FPS):
            progress = frame / (seconds * FPS - 1)
            if name == 'far':
                # Space perspective thresholds across the shot by walking a geometric series in distance.
                progress = 8. ** progress / 7. - 1. / 7.
            else:
                progress = progress * progress * (3. - 2. * progress)
            eye = tuple(start + progress * (end - start) for start, end in zip(a, b))
            seconds_elapsed = index / FPS
            gust = math.exp(-((seconds_elapsed - 1.35) / .53) ** 2)
            pressure = .29 + .07 * math.sin(seconds_elapsed * 3.) + .62 * gust
            yield name, (*eye, *target, seconds_elapsed, pressure, .85, .527, *sun, threshold)
            index += 1


def prepare(args):
    if not math.isfinite(args.threshold) or args.threshold <= 0:
        raise ValueError('The route requires a positive finite SSE threshold')
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'frames.tsv').write_text(''.join(' '.join(format(x, '.9g') for x in row) + '\n'
                                                  for _, row in route(args.threshold)))
    for channel in ('albedo', 'normal', 'orm'):
        layers = [dds_mips(args.textures / f'{stem}-{channel}.dds') for stem in ('bark', 'boughs')]
        size = 1024 if channel == 'orm' else 2048
        shapes = [(n, n, 4) for n in (size >> mip for mip in range(size.bit_length()))]
        if any([image.shape for image in layer] != shapes for layer in layers):
            raise ValueError(f'{channel}: expected both {size}×{size} DDS mip chains')
        with (args.output / f'{channel}.rgba').open('wb') as stream:
            for mip in zip(*layers):
                for image in mip:
                    stream.write(image.tobytes())


def record(args):
    args.output.mkdir(parents=True, exist_ok=False)
    frames = args.route_dir / 'frames.tsv'
    command = [args.pilot, 'route', args.shaders, args.asset, args.route_dir, args.mode, frames, args.output]
    video = args.output / 'route.mp4'
    encoder = ['ffmpeg', '-nostdin', '-v', 'error', '-f', 'rawvideo', '-pixel_format', 'rgb24',
               '-video_size', f'{WIDTH}x{HEIGHT}', '-framerate', str(FPS), '-i', 'pipe:0', '-an',
               '-c:v', 'libx264', '-threads', '4', '-preset', 'veryfast', '-crf', '15', '-pix_fmt', 'yuv420p',
               '-movflags', '+faststart', str(video)]
    started = time.monotonic()
    with (args.output / 'pilot.log').open('wb') as log, subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=log) as pilot:
        try:
            with subprocess.Popen(encoder, stdin=pilot.stdout) as encode:
                pilot.stdout.close()
                try:
                    status = encode.wait(timeout=112)
                    pilot_status = pilot.wait(timeout=max(0.1, 115 - (time.monotonic() - started)))
                finally:
                    if encode.poll() is None:
                        encode.kill()
            if status or pilot_status:
                raise RuntimeError(f'Capture failed: pilot {pilot_status}, ffmpeg {status}. See {log.name}')
        finally:
            if pilot.poll() is None:
                pilot.kill()
    expected = len(frames.read_text().splitlines())
    if len(read_samples(args.output)) != expected:
        raise ValueError('Pilot route metadata is incomplete')
    print(f'{args.mode}: {expected} frames in {time.monotonic() - started:.2f}s, {video}', flush=True)


def read_samples(output: Path) -> list[dict]:
    samples = [json.loads(line) for line in (output / 'route.jsonl').read_text().splitlines()]
    if any(row['frame'] != index for index, row in enumerate(samples)):
        raise ValueError(f'Unexpected frame numbering in {output}')
    return samples


def bounds(row, height: float):
    """Projection of the shared fir bounds on the downsampled frame."""
    eye, target = np.array(row[:3]), np.array(row[3:6])
    forward = target - eye
    forward /= np.linalg.norm(forward)
    right = np.cross((0, 1, 0), forward)
    right /= np.linalg.norm(right)
    up = np.cross(forward, right)
    corners = np.array([(x, y, z) for x in (-660, 660) for y in (0, height) for z in (-660, 660)]) - eye
    depth = np.maximum(corners @ forward, 50.)
    scale = .5 * PROJECTION * SAMPLE_HEIGHT / depth
    x = SAMPLE_WIDTH / 2 + (corners @ right) * scale
    y = SAMPLE_HEIGHT / 2 - (corners @ up) * scale
    return (max(0, math.floor(x.min())), max(0, math.floor(y.min())),
            min(SAMPLE_WIDTH, math.ceil(x.max())), min(SAMPLE_HEIGHT, math.ceil(y.max())))


def differences(video: Path, frames, height: float):
    command = ['ffmpeg', '-nostdin', '-v', 'error', '-i', video, '-vf',
               f'scale={SAMPLE_WIDTH}:{SAMPLE_HEIGHT}:flags=area', '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1']
    result = []
    before = before_shot = before_box = None
    with subprocess.Popen(command, stdout=subprocess.PIPE) as stream:
        for shot, row in frames:
            data = stream.stdout.read(SAMPLE_WIDTH * SAMPLE_HEIGHT)
            if len(data) != SAMPLE_WIDTH * SAMPLE_HEIGHT:
                raise ValueError(f'{video} ends after {len(result)} frames')
            image = np.frombuffer(data, np.uint8).reshape(SAMPLE_HEIGHT, SAMPLE_WIDTH).astype(np.int16)
            box = bounds(row, height)
            distance = 0.
            if shot == before_shot:
                x0, y0 = (min(a, b) for a, b in zip(box[:2], before_box[:2]))
                x1, y1 = (max(a, b) for a, b in zip(box[2:], before_box[2:]))
                distance = float(np.abs(image[y0:y1, x0:x1] - before[y0:y1, x0:x1]).mean())
            result.append(distance)
            before, before_shot, before_box = image, shot, box
        if stream.stdout.read(1) or stream.wait():
            raise ValueError(f'{video} contains unexpected frames or failed to decode')
    return result


def image_at(video: Path, frame: int, path: Path):
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', video, '-vf', f'select=eq(n\\,{frame})',
                    '-frames:v', '1', '-update', '1', '-y', path], check=True)


def report(args):
    rows = [tuple(map(float, line.split())) for line in (args.route_dir / 'frames.tsv').read_text().splitlines()]
    frames = list(zip((shot for shot, _ in route(1.)), rows, strict=True))
    opaque, cards = read_samples(args.opaque), read_samples(args.cards)
    if len(opaque) != len(frames) or len(cards) != len(frames):
        raise ValueError('Clip metadata does not cover all route frames')
    candidate = differences(args.opaque / 'route.mp4', frames, args.height_cm)
    control = differences(args.cards / 'route.mp4', frames, args.height_cm)
    events, key_frames = [], {}
    first = 0
    for shot, seconds, *_ in SHOTS:
        last = first + seconds * FPS
        key_frames[shot] = (first + last) // 2
        local = scores(candidate[first:last])
        control_scores = scores(control[first:last])
        for index in range(first + 1, last):
            old, new = opaque[index - 1], opaque[index]
            delta = new['cut_triangles'] - old['cut_triangles']
            terminal = new['terminal'] and not old['terminal']
            if not delta and not terminal:
                continue
            peak, around, jump = standout(local, index - first, 1.5)
            events.append({
                'shot': shot, 'frame': index, 'triangles_before': old['cut_triangles'],
                'triangles_after': new['cut_triangles'], 'delta_triangles': delta,
                'terminal_entry': terminal, 'peak_frame': first + peak, 'peak_ratio': local[peak],
                'surrounding_ratio': around, 'control_ratio': control_scores[peak],
                'adjacent_frame_delta': candidate[first + peak],
                'control_frame_delta': control[first + peak], 'jump': jump,
            })
        first = last
    far = [event for event in events if event['shot'] == 'far']
    for rank, event in enumerate(sorted(far, key=lambda event: -abs(event['delta_triangles']))[:4], 1):
        key_frames[f'lod-{rank:02}-before'] = event['frame'] - 1
        key_frames[f'lod-{rank:02}-after'] = event['frame']
    terminal = next((event for event in far if event['terminal_entry']), None)
    if terminal:
        key_frames.update(terminal_before=terminal['frame'] - 1, terminal_after=terminal['frame'])
    args.output.mkdir(parents=True, exist_ok=True)
    for label, frame in key_frames.items():
        for name, path in [('cards', args.cards), ('opaque', args.opaque)]:
            image_at(path / 'route.mp4', frame, args.output / f'{label}-{name}.png')
    summary = {
        'frames': len(frames), 'fps': FPS, 'terminal_seen': any(row['terminal'] for row in opaque),
        'terminal_crossing': terminal is not None, 'transitions': events, 'key_frames': key_frames,
    }
    (args.output / 'pops.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'{len(events)} cut changes, {sum(event["jump"] for event in events)} standout events, '
          f'terminal crossing: {terminal is not None}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(required=True)
    prepare_parser = subparsers.add_parser('prepare')
    prepare_parser.add_argument('textures', type=Path)
    prepare_parser.add_argument('output', type=Path)
    prepare_parser.add_argument('--threshold', type=float, default=11.)
    prepare_parser.set_defaults(run=prepare)
    record_parser = subparsers.add_parser('record')
    for argument in ('pilot', 'shaders', 'asset', 'route_dir'):
        record_parser.add_argument(argument, type=Path)
    record_parser.add_argument('mode', choices=('cards', 'opaque'))
    record_parser.add_argument('output', type=Path)
    record_parser.set_defaults(run=record)
    report_parser = subparsers.add_parser('report')
    for argument in ('route_dir', 'cards', 'opaque', 'output'):
        report_parser.add_argument(argument, type=Path)
    report_parser.add_argument('--height-cm', type=float, required=True)
    report_parser.set_defaults(run=report)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
