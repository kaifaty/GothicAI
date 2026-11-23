"""ctypes binding of native/liba2f_engine.so: the model is loaded once and runs one line at a time."""

import ctypes
import json
import math
from pathlib import Path

import numpy as np

NATIVE_LIB = Path(__file__).resolve().parents[2] / "native" / "build" / "liba2f_engine.so"
SAMPLE_RATE = 16000  # the network's input
NETWORK_FPS = 60  # the network's output; the service keeps every second frame


class EngineError(RuntimeError):
    pass


def model_info(model_dir: Path) -> tuple[list[str], list[str]]:
    """Emotion names (in the network's order) and identities of a diffusion model directory."""
    model = json.loads((model_dir / "model.json").read_text())
    info = json.loads((model_dir / model["networkInfoPath"]).read_text())
    return list(info["params"]["emotions"]), list(info["params"]["identities"])


class Engine:
    """Audio2Face-3D diffusion with the blendshape solver, as reports/.../a2f/a2f-weights.cpp runs it."""

    def __init__(self, model_dir: Path, face: str, gpu_solver: bool = False, lib: Path = NATIVE_LIB) -> None:
        self.emotions, identities = model_info(model_dir)
        if face not in identities:
            raise EngineError(f"face {face!r} is not in the model: {', '.join(identities)}")
        self._lib = ctypes.CDLL(str(lib))
        self._lib.a2f_open.restype = ctypes.c_void_p
        self._lib.a2f_open.argtypes = [
            ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t,
        ]
        self._lib.a2f_weight_count.restype = ctypes.c_size_t
        self._lib.a2f_weight_count.argtypes = [ctypes.c_void_p]
        self._lib.a2f_emotion_count.restype = ctypes.c_size_t
        self._lib.a2f_emotion_count.argtypes = [ctypes.c_void_p]
        self._lib.a2f_pose_name.restype = ctypes.c_char_p
        self._lib.a2f_pose_name.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        self._lib.a2f_run.restype = ctypes.c_longlong
        self._lib.a2f_run.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t,
        ]
        self._lib.a2f_close.argtypes = [ctypes.c_void_p]
        err = ctypes.create_string_buffer(1024)
        model = str(model_dir / "model.json").encode()
        self._handle = self._lib.a2f_open(model, identities.index(face), int(gpu_solver), err, len(err))
        if not self._handle:
            raise EngineError(err.value.decode(errors="replace"))
        count = self._lib.a2f_weight_count(self._handle)
        self.shapes = [self._lib.a2f_pose_name(self._handle, i).decode() for i in range(count)]
        if self._lib.a2f_emotion_count(self._handle) != len(self.emotions):
            raise EngineError("the network's emotions do not match network_info.json")

    def run(self, audio: np.ndarray, emotion: np.ndarray) -> np.ndarray:
        """Weights (frames x shapes) at 60 fps for 16 kHz mono float32 audio and one emotion vector."""
        audio = np.ascontiguousarray(audio, dtype=np.float32)
        emotion = np.ascontiguousarray(emotion, dtype=np.float32)
        capacity = math.ceil(len(audio) * NETWORK_FPS / SAMPLE_RATE) + 8
        out = np.empty((capacity, len(self.shapes)), dtype=np.float32)
        err = ctypes.create_string_buffer(1024)
        frames = self._lib.a2f_run(
            self._handle, audio.ctypes.data, len(audio), emotion.ctypes.data, out.ctypes.data, capacity,
            err, len(err),
        )
        if frames < 0:
            raise EngineError(err.value.decode(errors="replace"))
        return out[:frames]

    def close(self) -> None:
        if self._handle:
            self._lib.a2f_close(self._handle)
            self._handle = None
