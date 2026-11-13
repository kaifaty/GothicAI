"""Test edit of a region: region_face_from_nothing.py's triangle, put into the chunk 2_2 (it lands there, not in
1_1 below it)."""
import importlib.util
from pathlib import Path

PARAMETERS = {'chunk': '2_2'}

_spec = importlib.util.spec_from_file_location('region_face_from_nothing',
                                               Path(__file__).with_name('region_face_from_nothing.py'))
_base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_base)


def edit(ctx):
    _base.add_face(ctx, PARAMETERS['chunk'])
