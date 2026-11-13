"""Entry of the Blender side of world_edit.py (world_bpy.py, world_region_bpy.py for build-region): edits import
world_bpy as one module with it."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import world_bpy  # noqa: E402

job_path = sys.argv[sys.argv.index('--') + 1]
if 'region' in json.loads(Path(job_path).read_text()):
    import world_region_bpy  # noqa: E402
    world_region_bpy.main(job_path)
else:
    world_bpy.main(job_path)
