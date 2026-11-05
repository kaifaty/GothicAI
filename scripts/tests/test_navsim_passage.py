"""Host and CUDA passage parity against the engine, without installed game data."""
from pathlib import Path
import sys

import numpy as np
import pytest

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(SCRIPTS / "navsim" / "gpumem"))


@pytest.mark.parametrize("backend", ("host", "cuda"))
def test_passage_matches_engine_after_replayed_updates(tmp_path, backend):
    import torch

    if backend == "cuda" and not torch.cuda.is_available():
        pytest.skip("CUDA passage parity requires a CUDA GPU; host passage parity runs separately")
    import bench
    import captures
    import gpumem
    import passage
    from nav_bc_data import HD_PREPROCESSING, PACE_VECTOR_SIZE, pace_preprocessing
    from nav_bc_model import NavPolicy, export_weights

    weights = tmp_path / "policy.navbc"
    export_weights(NavPolicy(PACE_VECTOR_SIZE, hidden_size=16, format_version=5), weights,
                   pace_preprocessing(HD_PREPROCESSING))
    capture = tmp_path / "memory-cost.bin"
    captures.write(capture, captures.synthetic(seed=5, updates=24))
    # Retain the original replay and add a real near height discontinuity:
    # the two ground hits are one 25 cm cell apart and differ by 150 cm.
    step = tmp_path / "step.bin"
    pose = np.zeros(1, captures.POSE)
    pose["forward"], pose["ms"] = (1, 0, 0), 1000
    pose["step"], pose["slope"], pose["body"] = 60, 50, 180
    rays = np.zeros(476, captures.RAY)
    rays["max_range"] = 7000  # Zero-distance slots do not update memory, but still encode a valid eye.
    ground = rays[:2]
    ground["point"] = ((112.5, 0, 112.5), (137.5, 150, 112.5))
    ground["origin"], ground["direction"] = ground["point"] + (0, 200, 0), (0, -1, 0)
    ground["distance"], ground["normal_y"] = 200, 1
    ground["kind"], ground["hit"], ground["complete"] = 1, 1, 1
    ground["time"], ground["near_time"] = 1000, 1000
    captures.write(step, captures.Capture(str(step), np.zeros(0, captures.TILE), np.zeros(0, np.uint64),
                                        476, pose, np.array([0, 476], np.int64), np.arange(476, dtype=np.uint16),
                                        rays, np.ones(1, np.int32)))
    reference = tmp_path / "libnavmemref.so"
    bench.build_reference(SCRIPTS.parent, reference)
    bench.Reference.library = None
    result = passage.run_parity(gpumem.Backend(backend, tmp_path / "build"),
                                [captures.read(capture), captures.read(step)],
                                reference, weights, per_view=192)
    assert result["compared"] == result["equal"] == 1728
    assert result["first_mismatch"] is None and result["state_errors"] == 0
    assert not result["missing_coverage"]
