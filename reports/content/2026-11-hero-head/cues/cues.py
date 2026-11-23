"""Compatibility entry point for the offline cue tracks."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'character-tool' / 'src'))
from character_tool.cli import main

if __name__ == '__main__':
    sys.exit(main(['face', 'cues', *sys.argv[1:]]))
