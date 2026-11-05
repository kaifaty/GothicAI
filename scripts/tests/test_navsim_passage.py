"""GPU passage parity against the engine, independent of installed game data."""
from pathlib import Path
import sys

import pytest

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(SCRIPTS / "navsim" / "gpumem"))


def test_passage_matches_engine_after_replayed_updates(tmp_path):
    torch = pytest.importorskip("torch", reason="CUDA passage parity requires PyTorch with CUDA")
    if not torch.cuda.is_available():
        pytest.skip("CUDA passage parity requires a CUDA GPU; CPU contract tests run separately")
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
    reference = tmp_path / "libnavmemref.so"
    bench.build_reference(SCRIPTS.parent, reference)
    result = passage.run_parity(gpumem.Backend("cuda", tmp_path / "build"), [captures.read(capture)],
                                reference, weights, per_view=192)
    assert result["compared"] == result["equal"] == 1536
    assert result["first_mismatch"] is None and result["state_errors"] == 0
    assert result["targeted_live_hazard_segments"]["water"] > 0
