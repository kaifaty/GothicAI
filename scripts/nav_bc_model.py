#!/usr/bin/env python3
"""Small recurrent walking policy and its portable, single-file weight export.

Torch is required by this training module only. Runtime inference lives in
``nav_bc_numpy.py`` and does not import this module or torch.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

import torch
from torch import Tensor, nn


HEAD_SIZES = {"forward": 3, "strafe": 3, "turn": 3, "walk": 2, "jump": 2, "climb": 2}
BC3_HEAD_SIZES = {"forward": 3, "strafe": 3, "turn": 3, "walk": 2, "maneuver": 3}
FORMAT_NAME = "gothic-nav-bc"
FORMAT_VERSION = 1


class NavPolicy(nn.Module):
    """Encode the two local maps and sensors, then retain state with one GRU.

    Map inputs have shape ``(batch, time, channels, height, width)``; the vector has
    shape ``(batch, time, vector_size)``. Hidden state follows torch's GRU
    convention: ``(1, batch, hidden_size)``. Every output logit tensor has
    shape ``(batch, time, classes)``.
    """

    def __init__(self, vector_size: int, hidden_size: int = 256,
                 format_version: int = 1, auxiliary: bool = False) -> None:
        super().__init__()
        if vector_size <= 0 or hidden_size <= 0:
            raise ValueError("vector_size and hidden_size must be positive")
        self.vector_size = int(vector_size)
        self.hidden_size = int(hidden_size)
        if format_version not in (1, 2, 3, 4):
            raise ValueError("unsupported navigation weight format")
        self.format_version = format_version
        self.head_sizes = HEAD_SIZES if format_version == 1 else BC3_HEAD_SIZES
        channels = 11 if format_version == 4 else 10
        self.near_shape = (channels, 64, 64) if format_version >= 3 else (9, 32, 32)
        self.far_shape = (channels, 50, 100) if format_version >= 3 else (9, 25, 25)
        self.auxiliary = auxiliary
        self.near_encoder = self._map_encoder(self.near_shape[0])
        self.far_encoder = self._map_encoder(self.far_shape[0])
        self.vector_encoder = nn.Sequential(nn.Linear(vector_size, 128), nn.ReLU())
        self.fusion = nn.Sequential(nn.Linear(256, 128), nn.ReLU())
        self.gru = nn.GRU(128, hidden_size, num_layers=1, batch_first=True)
        # "forward" itself cannot be a ModuleDict key: nn.Module owns that name.
        for name, classes in self.head_sizes.items():
            setattr(self, f"head_{name}", nn.Linear(hidden_size, classes))
        if auxiliary:
            self.aux_stuck = nn.Linear(hidden_size, 1)
            self.aux_next_near = nn.Linear(hidden_size, 9 * 8 * 8)
            self.aux_detour = nn.Linear(hidden_size, 4)
        if format_version < 3 and self.mac_count()["total"] > 2_000_000:
            raise ValueError("policy exceeds the 2,000,000 MAC decision budget")

    @staticmethod
    def _map_encoder(channels: int) -> nn.Sequential:
        return nn.Sequential(
            nn.Conv2d(channels, 8, kernel_size=3, stride=2, padding=1),
            nn.ReLU(),
            nn.Conv2d(8, 16, kernel_size=3, stride=2, padding=1),
            nn.ReLU(),
            nn.AdaptiveAvgPool2d((2, 2)),
            nn.Flatten(),
        )

    def forward(
        self, near: Tensor, far: Tensor, vector: Tensor, hidden: Tensor | None = None,
        return_auxiliary: bool = False,
    ) -> tuple[dict[str, Tensor], Tensor]:
        batch, time = vector.shape[:2]
        near_features = self.near_encoder(near.reshape(batch * time, *self.near_shape))
        far_features = self.far_encoder(far.reshape(batch * time, *self.far_shape))
        vector_features = self.vector_encoder(vector.reshape(batch * time, self.vector_size))
        features = self.fusion(torch.cat((near_features, far_features, vector_features), dim=-1))
        recurrent, hidden = self.gru(features.reshape(batch, time, 128), hidden)
        logits = {name: getattr(self, f"head_{name}")(recurrent) for name in self.head_sizes}
        if return_auxiliary:
            if not self.auxiliary:
                raise ValueError("policy has no training auxiliary heads")
            auxiliary = {
                "stuck": self.aux_stuck(recurrent).squeeze(-1),
                "next_near": nn.functional.interpolate(
                    self.aux_next_near(recurrent).reshape(batch * time, 9, 8, 8),
                    size=(32, 32), mode="bilinear", align_corners=False).reshape(batch, time, 9, 32, 32),
                "detour": self.aux_detour(recurrent),
            }
            return logits, hidden, auxiliary
        return logits, hidden

    def mac_count(self) -> dict[str, Any]:
        """Matrix-product MACs per decision, including zero-padded conv taps.

        Bias addition, activation, pooling, and scalar GRU gates are listed as
        non-MAC operations rather than silently counted as dense matrix work.
        """
        near1 = tuple((size + 1) // 2 for size in self.near_shape[1:])
        near2 = tuple((size + 1) // 2 for size in near1)
        far1 = tuple((size + 1) // 2 for size in self.far_shape[1:])
        far2 = tuple((size + 1) // 2 for size in far1)
        components = {
            "near_conv1": math.prod(near1) * 8 * self.near_shape[0] * 3 * 3,
            "near_conv2": math.prod(near2) * 16 * 8 * 3 * 3,
            "far_conv1": math.prod(far1) * 8 * self.far_shape[0] * 3 * 3,
            "far_conv2": math.prod(far2) * 16 * 8 * 3 * 3,
            "vector_linear": self.vector_size * 128,
            "fusion_linear": 256 * 128,
            "gru_input": 3 * self.hidden_size * 128,
            "gru_hidden": 3 * self.hidden_size * self.hidden_size,
            "heads": self.hidden_size * sum(self.head_sizes.values()),
        }
        return {
            "total": sum(components.values()),
            "components": components,
            "convention": "One multiply plus accumulate per convolution/linear/GRU matrix weight; includes padded zero taps.",
            "other_operations": {
                "gru_elementwise_multiplies": 3 * self.hidden_size,
                "pool_output_means": 2 * 16 * 2 * 2,
                "pool_input_values_summed": 16 * sum(
                    math.prod(2 * ((size + 1) // 2) for size in shape)
                    for shape in (near2, far2)),
                "excluded": "bias additions, ReLU, sigmoid, tanh, pooling reductions, scalar gate additions",
            },
        }

    def architecture(self) -> dict[str, Any]:
        """A runtime-independent description sufficient to reproduce inference."""
        return {
            "name": "map-cnn-vector-gru",
            "version": 1,
            "inputs": {
                "near": list(self.near_shape),
                "far": list(self.far_shape),
                "vector": [self.vector_size],
                "dtype": "float32",
                "map_layout": "CHW",
            },
            "map_encoders": {
                "separate_weights": True,
                "layers": [
                    {"type": "conv2d", "in_channels": self.near_shape[0], "out_channels": 8, "kernel": 3, "stride": 2, "padding": 1, "activation": "relu"},
                    {"type": "conv2d", "in_channels": 8, "out_channels": 16, "kernel": 3, "stride": 2, "padding": 1, "activation": "relu"},
                    {"type": "adaptive_avg_pool2d", "output": [2, 2], "bins": "start=floor(i*input/output); end=ceil((i+1)*input/output)"},
                    {"type": "flatten", "order": "CHW", "output": 64},
                ],
            },
            "vector_encoder": {"in": self.vector_size, "out": 128, "activation": "relu"},
            "fusion": {"concat": ["near", "far", "vector"], "in": 256, "out": 128, "activation": "relu"},
            "gru": {
                "input_size": 128,
                "hidden_size": self.hidden_size,
                "layers": 1,
                "gate_order": ["reset", "update", "new"],
                "equations": [
                    "r = sigmoid(W_ir*x + b_ir + W_hr*h + b_hr)",
                    "z = sigmoid(W_iz*x + b_iz + W_hz*h + b_hz)",
                    "n = tanh(W_in*x + b_in + r*(W_hn*h + b_hn))",
                    "h_new = (1-z)*n + z*h",
                ],
                "initial_hidden": "zeros",
            },
            "heads": {
                name: {"classes": classes, "values": [0, 1, 2] if name == "maneuver" else
                       ([-1, 0, 1] if classes == 3 else [0, 1])}
                for name, classes in self.head_sizes.items()
            },
            "macs": self.mac_count(),
        }


def export_weights(
    model: NavPolicy, path: str | Path, preprocessing: dict[str, Any] | None = None,
    temperatures: dict[str, float] | None = None,
) -> dict[str, Any]:
    """Write one UTF-8 JSON line followed immediately by float32 tensor data.

    Tensor byte offsets are relative to the first byte following the header's
    terminating newline. Tensors use little-endian float32 in C order, with
    torch's native weight layout (linear OI, convolution OIHW).
    """
    tensors = []
    payload = []
    offset = 0
    for name, tensor in model.state_dict().items():
        if name.startswith("aux_"):
            continue
        array = tensor.detach().cpu().numpy().astype("<f4", copy=False)
        data = array.tobytes(order="C")
        tensors.append({"name": name, "shape": list(array.shape), "dtype": "float32", "offset": offset, "nbytes": len(data)})
        payload.append(data)
        offset += len(data)
    header = {
        "format": FORMAT_NAME,
        "version": model.format_version,
        "byte_order": "little",
        "offset_origin": "first byte after JSON header newline",
        "data_nbytes": offset,
        "architecture": model.architecture(),
        "preprocessing": preprocessing or {},
        "tensors": tensors,
    }
    if temperatures is not None:
        if set(temperatures) != set(model.head_sizes) or any(
                not math.isfinite(value) or value <= 0 for value in temperatures.values()):
            raise ValueError("temperatures must contain one positive finite value per action head")
        header["temperatures"] = temperatures
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as stream:
        stream.write(json.dumps(header, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n")
        for data in payload:
            stream.write(data)
    return header
