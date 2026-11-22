#!/usr/bin/env python3
"""Extract game speech (wav) or convert Audio2Face weights to a face track (json)."""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'character-tool' / 'src'))
from character_tool.cli import main as character_tool
from character_tool.face.face_track import EMOTIONS, FPS, RATE, SHAPES, emotion_label, track, vdf_files, wav


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('wav', 'json'))
    operation = parser.parse_args(argv[:1]).operation
    return character_tool(['face', operation, *argv[1:]])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
