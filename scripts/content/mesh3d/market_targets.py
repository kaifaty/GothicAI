#!/usr/bin/env python3
"""Measure scalar targets for the market set (stage 2, NW_CITY_MERCHANT).

    python3 scripts/content/mesh3d/market_targets.py --game "<Gothic II>" [--extractor target-measures]

Same decoder and record shape as measure_targets.py (bbox in glTF metres, origin,
source triangles, NewWorld instances); only names and numbers leave memory.
The 21-visual set minus the pilot's barrel and crate, which the pilot builds.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_targets import build_extractor, target_record  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]


def zone(key: str, group: str, role: str) -> dict:
    return {"key": key, "group": group, "role": role}


WOOD_PLANK = zone("OWN_PROP_WOOD_PLANK", "WOOD", "sawn boards and planks")
WOOD_FRAME = zone("OWN_PROP_WOOD_FRAME", "WOOD", "posts, rails and frames")
IRON = zone("OWN_PROP_IRON_BAND", "METAL", "forged iron straps, hoops, brackets and nails")
STEEL = zone("OWN_PROP_STEEL_BLADE", "METAL", "polished steel blades and heads")
CLOTH = zone("OWN_PROP_CLOTH", "UNDEF", "woven cloth: canvas, sacking, curtains")
ROPE = zone("OWN_PROP_ROPE", "UNDEF", "twisted hemp rope")

TARGETS = [
    {"id": "weapon-barrel", "visual": "NW_CITY_WEAPON_BARREL_01.MRM", "budget_tris": 3000,
     "zones": [zone("OWN_PROP_WOOD_STAVE", "WOOD", "barrel staves and head"), IRON, STEEL,
               zone("OWN_PROP_WOOD_HAFT", "WOOD", "hilts, grips and hafts")]},
    {"id": "sign", "visual": "NW_MISC_SIGN_01.MRM", "budget_tris": 1000, "zones": [WOOD_FRAME, WOOD_PLANK, IRON]},
    {"id": "oil-lamp", "visual": "NW_CITY_OILLAMP_01.MRM", "budget_tris": 1000,
     "zones": [zone("OWN_PROP_BRASS", "METAL", "lamp body, collar and bracket"),
               zone("OWN_PROP_GLASS", "UNDEF", "glass chimney"), WOOD_FRAME]},
    {"id": "barrel-group", "visual": "NW_HARBOUR_BARRELGROUP_01.MRM", "budget_tris": 6000,
     "zones": [zone("OWN_PROP_WOOD_STAVE", "WOOD", "barrel staves and head"), IRON, WOOD_FRAME]},
    {"id": "rope", "visual": "NW_HARBOUR_ROPE_01.MRM", "budget_tris": 4000, "zones": [ROPE]},
    {"id": "candle", "visual": "NW_CITY_CANDLE_01.MRM", "budget_tris": 2700,
     "zones": [IRON, zone("OWN_PROP_WAX", "UNDEF", "candle wax and drips")]},
    {"id": "weapon-bag", "visual": "NW_CITY_WEAPON_BAG_01.MRM", "budget_tris": 1500,
     "zones": [zone("OWN_PROP_LEATHER", "UNDEF", "leather and cloth wrapping"), STEEL,
               zone("OWN_PROP_WOOD_HAFT", "WOOD", "hilts, grips and hafts")]},
    {"id": "crate-pile", "visual": "NW_HARBOUR_CRATEPILE_01.MRM", "budget_tris": 6000,
     "zones": [WOOD_PLANK, zone("OWN_PROP_WOOD_BRACE", "WOOD", "crate end frames and cleats"),
               zone("OWN_PROP_ROPE", "UNDEF", "rope handles on crate ends"),
               zone("OWN_PROP_WOOD_INNER", "WOOD", "dark core box seen through the plank gaps")]},
    {"id": "deco-shield", "visual": "NW_CITY_DECO_SHIELD_01.MRM", "budget_tris": 1500,
     "zones": [zone("OWN_PROP_WOOD_PAINTED", "WOOD", "shield boards painted dull red"),
               zone("OWN_PROP_WOOD_PAINTED_LIGHT", "WOOD", "shield boards painted off-white"), IRON, STEEL,
               zone("OWN_PROP_WOOD_HAFT", "WOOD", "hilts and grips")]},
    {"id": "window-in", "visual": "NW_CITY_WINDOW_WOOD_IN_01.MRM", "budget_tris": 2000,
     "zones": [WOOD_FRAME, zone("OWN_PROP_GLASS", "UNDEF", "leaded glass panes"), IRON, CLOTH]},
    {"id": "window-out", "visual": "NW_CITY_WINDOW_WOOD_OUT_01.MRM", "budget_tris": 1200,
     "zones": [WOOD_FRAME, zone("OWN_PROP_GLASS", "UNDEF", "leaded glass panes"), IRON]},
    {"id": "cupboard", "visual": "NW_CITY_CUPBOARD_POOR_02.MRM", "budget_tris": 2000, "zones": [WOOD_PLANK, WOOD_FRAME, IRON]},
    {"id": "wolf-head", "visual": "NW_CITY_DECO_WOLFHEAD_01.MRM", "budget_tris": 2500,
     "zones": [WOOD_PLANK, zone("OWN_PROP_FUR", "UNDEF", "wolf fur"), zone("OWN_PROP_BONE", "UNDEF", "teeth"),
               zone("OWN_PROP_EYE", "UNDEF", "amber glass eyes"), zone("OWN_PROP_NOSE", "UNDEF", "black nose")]},
    {"id": "stall-roof", "visual": "NW_CITY_MARKETSTALL_ROOF_01.MRM", "budget_tris": 2500, "zones": [WOOD_FRAME, CLOTH, ROPE]},
    {"id": "firewood", "visual": "OC_FIREWOOD_V3.MRM", "budget_tris": 3000,
     "zones": [zone("OWN_PROP_BARK", "WOOD", "log bark"), zone("OWN_PROP_WOOD_END", "WOOD", "sawn log ends"),
               zone("OWN_PROP_WOOD_SPLIT", "WOOD", "split faces along the grain")]},
    {"id": "sacks", "visual": "OC_SAECKE_01.MRM", "budget_tris": 2500, "zones": [CLOTH, ROPE]},
    {"id": "carpet", "visual": "NW_CITY_CARPET_03.MRM", "budget_tris": 600,
     "zones": [zone("OWN_PROP_CARPET", "UNDEF", "woven carpet with its own pattern")]},
    {"id": "cart", "visual": "NW_CITY_CART_01.MRM", "budget_tris": 3000, "zones": [WOOD_PLANK, WOOD_FRAME, IRON]},
    {"id": "counter", "visual": "NW_CITY_COUNTER_01.MRM", "budget_tris": 2500, "zones": [WOOD_PLANK, WOOD_FRAME, IRON]},
]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--extractor", type=Path, help="prebuilt target-measures binary")
    parser.add_argument("--zenkit-build", type=Path,
                        default=Path(tempfile.gettempdir()) / "gothic-original-scalar-measures/zenkit")
    parser.add_argument("--output", type=Path, default=ROOT / "data/content/mesh3d-market.json")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gothic-market-measures-") as temp:
        extractor = args.extractor or Path(temp) / "target-measures"
        if args.extractor is None:
            build_extractor(args.zenkit_build, extractor)
        result = subprocess.run([str(extractor), str(args.game), *(t["visual"] for t in TARGETS)],
                                check=True, capture_output=True, text=True)
    measurements = json.loads(result.stdout)
    if len(measurements) != len(TARGETS):
        raise ValueError(f"decoder must return exactly {len(TARGETS)} targets")
    records = [target_record({**t, "kind": "static_mrm"}, m) for t, m in zip(TARGETS, measurements)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(records, indent=2, ensure_ascii=False) + "\n")
    print(f"wrote {len(records)} scalar targets to {args.output}")


if __name__ == "__main__":
    main()
