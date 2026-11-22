"""Compatibility entry point for the head builder in this checkout."""
import sys
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(repo / 'character-tool/src'))
from character_tool.head import build_head

if __name__ == '__main__':
    build_head.main()
else:
    sys.modules[__name__] = build_head
