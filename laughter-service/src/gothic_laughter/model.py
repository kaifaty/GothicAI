"""Pinned MIT-licensed zencastr/laughter-detection model and its ONNX inputs."""

import hashlib
from pathlib import Path

import httpx
import numpy as np

MODEL_REVISION = "849623f8338341d7e07be00a487337e0086ec14d"
MODEL_URL = f"https://huggingface.co/zencastr/laughter-detection/resolve/{MODEL_REVISION}/model.onnx"
MODEL_SHA256 = "fa6445a9738c66b319d36b716d860b21703fba1fe41e1137e82ca288b04cb74f"
MODEL_BYTES = 362_287_206
DEFAULT_MODEL = Path.home() / ".local/share/gothic-ai/laughter/849623f83383/model.onnx"
WINDOW = 64_000


def fetch_model(path: Path = DEFAULT_MODEL) -> Path:
    """Download the pinned model if absent, verifying it before publishing the file."""
    path = Path(path).expanduser()
    if path.is_file():
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(f"{path.name}.part")
    digest = hashlib.sha256()
    size = 0
    try:
        with httpx.Client(follow_redirects=True, timeout=60.0) as client:
            with client.stream("GET", MODEL_URL) as response:
                response.raise_for_status()
                with partial.open("wb") as output:
                    for chunk in response.iter_bytes(chunk_size=1024 * 1024):
                        output.write(chunk)
                        digest.update(chunk)
                        size += len(chunk)
        if size != MODEL_BYTES or digest.hexdigest() != MODEL_SHA256:
            raise ValueError(f"laughter model verification failed: {size} bytes, sha256 {digest.hexdigest()}")
        partial.replace(path)
        return path
    except BaseException:
        partial.unlink(missing_ok=True)
        raise


def load_session(model: Path, device: str, threads: int):
    # Imported only when loading the real model; HTTP tests need neither ORT nor weights.
    import onnxruntime as ort

    path = fetch_model(model)
    options = ort.SessionOptions()
    if device == "cuda":
        ort.preload_dlls()
        providers = [("CUDAExecutionProvider", {"arena_extend_strategy": "kSameAsRequested"})]
    else:
        options.intra_op_num_threads = threads
        providers = ["CPUExecutionProvider"]
    return ort.InferenceSession(str(path), sess_options=options, providers=providers)


def score_window(session, audio: np.ndarray) -> float:
    """Zero-pad one window while preserving the number of genuine samples."""
    waveform = np.zeros((1, WINDOW), dtype=np.float32)
    waveform[0, :len(audio)] = audio
    result = session.run(
        ["clip_prob"],
        {"waveform": waveform, "n_valid": np.array([len(audio)], dtype=np.int64)},
    )
    probability = float(np.asarray(result[0]).reshape(-1)[0])
    if not np.isfinite(probability):
        raise ValueError("non-finite laughter probability")
    return probability
