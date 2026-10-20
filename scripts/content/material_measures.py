#!/usr/bin/env python3
"""Measure our PNGs with the exact C++ function used by the original decoder.

The small standard-library-only shared library is cached outside the repository.
No game data enters Python: texture-luminance.cpp calls the same header in memory.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image


DIRECTORY = Path(__file__).resolve().parent
_library = None


def _core():
    global _library
    if _library is None:
        source = DIRECTORY / "material-measures.cpp"
        digest = hashlib.sha256(source.read_bytes() + (DIRECTORY / "material-measures.h").read_bytes()).hexdigest()[:20]
        cache = Path(tempfile.gettempdir()) / f"gothic-material-measures-{os.getuid()}"
        cache.mkdir(mode=0o700, exist_ok=True)
        target = cache / (digest + (".dylib" if sys.platform == "darwin" else ".so"))
        if not target.exists():
            with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
                temporary = Path(stream.name)
            try:
                subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-shared", "-fPIC",
                                str(source), "-o", str(temporary)], check=True, capture_output=True, text=True)
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
        _library = ctypes.CDLL(str(target))
        _library.material_measure_rgba.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int,
                                                  ctypes.c_int, ctypes.POINTER(ctypes.c_double)]
        _library.material_measure_rgba.restype = ctypes.c_int
    return _library


def measure_material(image, *, uv_m_per_unit=None):
    """Return scalar periods, direction, mean OKLab and log contrast/detail.

    Input is our PIL image, PNG path or uint8 RGB(A) array. U is image x, V image
    y. Period zero means no dominant spectral peak. Direction describes the
    length of stripes/planks (horizontal stripes run along U).
    """
    if isinstance(image, (str, Path)):
        with Image.open(image) as opened:
            return measure_material(opened, uv_m_per_unit=uv_m_per_unit)
    if isinstance(image, Image.Image):
        array = np.asarray(image.convert("RGBA"), dtype=np.uint8)
    else:
        array = np.asarray(image)
        if array.dtype != np.uint8 or array.ndim != 3 or array.shape[2] not in (3, 4):
            raise ValueError("expected uint8 RGB or RGBA image")
        if array.shape[2] == 3:
            array = np.concatenate((array, np.full((*array.shape[:2], 1), 255, dtype=np.uint8)), axis=2)
    array = np.ascontiguousarray(array)
    values = (ctypes.c_double * 17)()
    if _core().material_measure_rgba(array.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                                     array.shape[1], array.shape[0], values):
        raise ValueError("material has no pixels surviving alpha cutoff or invalid dimensions")
    result = dict(period_u_uv=values[0], period_v_uv=values[1], period_u_confidence=values[2],
                  period_v_confidence=values[3], direction=("none", "U", "V")[int(values[4])],
                  direction_confidence=values[5], direction_angle_deg=values[6],
                  oklab=list(values[7:10]), mean_linear_rgb=list(values[10:13]),
                  log_spread=values[13], detail_k3=values[14],
                  detail_k9=values[15], detail_k27=values[16])
    if uv_m_per_unit is not None:
        if not np.isfinite(uv_m_per_unit) or uv_m_per_unit <= 0:
            raise ValueError("uv_m_per_unit must be finite and positive")
        result.update(period_u_m=result["period_u_uv"] * uv_m_per_unit,
                      period_v_m=result["period_v_uv"] * uv_m_per_unit)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--uv-m-per-unit", type=float)
    args = parser.parse_args(argv)
    print(json.dumps(measure_material(args.image, uv_m_per_unit=args.uv_m_per_unit), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
