"""Price the archived tier coverage and show sensitivity to replay and ordering costs."""

import argparse
import json
from pathlib import Path


TRIANGLE_MS = 0.45e-6
DEPTH_TRIANGLE_MS = (2.651 / 6_213_700, TRIANGLE_MS)
REPLAY_OVERHEAD_MS = (0.03, 0.10)
PREFIX_OVERHEAD_MS = (0.03, 0.10)
NODE_REPLAY_OVERHEAD_MS = (0.05, 0.12)


def read(path):
    return json.loads(path.read_text())


def paired_inputs(folder, scene):
    snapshot = read(folder / f"{scene}.json")
    model = read(folder / f"{scene}-model.json")
    tier = model["variants"]["full_tier"]
    return {
        "triangles": snapshot["boxes"]["full_tier"]["triangles"],
        "draw_ms": tier["saved_gbuffer_ms"],
        "walk_ms": tier["saved_walk_ms"],
        "test_ms": [tier["test_projection_ms"], tier["test_texel_ms"]],
        "fresh_pyramid_ms": model["calibration"]["pyramid"]["fresh_ms"],
    }


def replay(gross, coverage, seed_triangles, test, overhead):
    return [
        gross * coverage[0] - seed_triangles[1] * DEPTH_TRIANGLE_MS[1] - test[1] - overhead[1],
        gross * coverage[1] - seed_triangles[0] * DEPTH_TRIANGLE_MS[0] - test[0] - overhead[0],
    ]


def prefix(inputs, coverage):
    # The existing stream must be expanded to identify a true compact prefix.
    # Its savings are raster only and the stock 3b pyramid is already spent.
    draw, pyramid, test = inputs["draw_ms"], inputs["fresh_pyramid_ms"], inputs["test_ms"]
    return [draw * coverage[0] - pyramid - test[1] - PREFIX_OVERHEAD_MS[1],
            draw * coverage[1] - pyramid - test[0] - PREFIX_OVERHEAD_MS[0]]


def estimate(archive, coverage_file):
    group = archive / "tree-8ms-cull-granularity/result"
    inside = paired_inputs(group, "inside")
    walk = paired_inputs(group, "walk")
    b1 = read(archive / "tree-8ms-b1-proto/output-abba/partial-a1-b1.json")["scenes"]["under-low"]
    node_rejects = b1["B1"]["counts"]["tree_bough_hiz_rejected_triangles"]["mean"]
    cut_ms = b1["A1"]["pass_ms"]["Bough-cut"]["p50"]
    low = {
        "triangles": node_rejects * 0.92,
        "draw_ms": node_rejects * 0.92 * TRIANGLE_MS,
        "walk_ms": cut_ms * 0.38,
        "test_ms": walk["test_ms"],
        # Only the two-pyramid total is available here.
        "fresh_pyramid_ms": b1["B1"]["pass_ms"]["Bough-HiZ-pyramid"]["p50"],
    }

    census = read(coverage_file)["census"]
    census_cases = {}
    for name, case in census["cases"].items():
        sample = case["with_other_final"]
        seed = case["seed_triangles"]
        gross = sample["triangles"] * TRIANGLE_MS + sample["visits"] / census["total_visits"] * 0.610304
        texel_cost = inside["test_ms"][1] * sample["texels"] / 96_190
        test = (inside["test_ms"][0], max(inside["test_ms"][0], texel_cost))
        census_cases[name] = {
            "gross_ms_if_preserved": gross,
            "net_ms_at_75_to_100_percent": replay(gross, (0.75, 1.0), (seed, seed), test, REPLAY_OVERHEAD_MS),
            "needed_fraction_at_045_with_003_overhead": (0.5 + seed * DEPTH_TRIANGLE_MS[1] + test[1]
                                              + REPLAY_OVERHEAD_MS[0]) / gross,
        }

    scenarios = {}
    for name, inputs, fraction in (("inside", inside, (0.50, 1.0)),
                                   ("walk", walk, (0.30, 0.80)),
                                   ("low_sun", low, (0.30, 0.80))):
        gross = inputs["draw_ms"] + inputs["walk_ms"]
        scenarios[name] = {
            "final_depth_calibration": inputs,
            "strict_prefix_ms": prefix(inputs, fraction),
            "winning_node_replay_ms": replay(gross, fraction, (150_000, 300_000),
                                             inputs["test_ms"], NODE_REPLAY_OVERHEAD_MS),
        }
        if name != "inside":
            scenarios[name]["full_replay_ms"] = replay(gross, fraction, (278_338, 763_999),
                                                        inputs["test_ms"], REPLAY_OVERHEAD_MS)
    return {"census": census_cases, "sensitivity": scenarios}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("coverage", type=Path)
    args = parser.parse_args()
    print(json.dumps(estimate(args.archive, args.coverage), indent=2))


if __name__ == "__main__":
    main()
