"""Entry of the Blender side of world_edit.py (world_bpy.py): edits import world_bpy as one module with it."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import world_bpy  # noqa: E402

world_bpy.main(sys.argv[sys.argv.index('--') + 1])
