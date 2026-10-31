"""Import check without weights/inference; FlexGEMM queries the CUDA device."""
import argparse
import json
from pathlib import Path

from adapter import assert_clean_imports, pipeline_class


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--model", choices=["trellis2", "trellis"], required=True)
    args = parser.parse_args()
    cls = pipeline_class(args.root.expanduser().resolve(), args.model)
    import torch
    print(json.dumps({"model": args.model, "pipeline_class": cls.__name__, "cuda_initialized": torch.cuda.is_initialized(),
                      **assert_clean_imports()}))
