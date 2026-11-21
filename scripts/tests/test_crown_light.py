import re
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("name", ["ShadeFloor", "OpenTop", "SkyLobe"])
def test_shader_matches_crown_light_reference(name):
    shader = ROOT / "OpenGothic/shader/lighting/crown_light.glsl"
    reference = ROOT / "OpenGothic/common/graphics/crownlight.h"

    def constant(path, symbol):
        source = path.read_text(encoding="utf-8")
        match = re.search(rf"^\s*(?:static constexpr|const)\s+float\s+{symbol}\s*=\s*([^;]+);", source, re.M)
        assert match, f"Missing {symbol} in {path.name}"
        return float(match[1].strip().removesuffix("f"))

    assert constant(shader, "Crown" + name) == constant(reference, name)
