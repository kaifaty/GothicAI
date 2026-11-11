"""Summarise res-<label>.jsonl from bench.py: python3 analyze.py <dir>"""
import json
import statistics as st
import sys
from pathlib import Path


def q(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p))] if xs else float("nan")


def summary(path):
    with open(path, encoding="utf-8") as f:
        rows = [json.loads(line) for line in f]
    cases = [r for r in rows if "i" in r and "error" not in r]
    errors = [r for r in rows if "error" in r]
    conc = [r for r in rows if "concurrent" in r]
    t = lambda r, k: r[k]["timings"]
    fw_sent = [r["fw"]["first_sentence_ms"] for r in cases]
    fw_tok = [r["fw"]["first_token_ms"] for r in cases]
    fw_prompt = [t(r, "fw").get("prompt_ms", 0) for r in cases]
    fw_pn = [t(r, "fw").get("prompt_n", 0) for r in cases]
    warm_ms = [t(r, "warm").get("prompt_ms", 0) for r in cases]
    warm_n = [t(r, "warm").get("prompt_n", 0) for r in cases]
    line_rate = [t(r, "line").get("predicted_per_second", 0) for r in cases if t(r, "line").get("predicted_n", 0) > 4]
    line_n = [t(r, "line").get("predicted_n", 0) for r in cases]
    line_total = [r["line"]["total_ms"] for r in cases]
    line_sent = [r["line"]["first_sentence_ms"] for r in cases]
    dn = sum(t(r, k).get("draft_n", 0) for r in cases for k in ("fw", "line"))
    da = sum(t(r, k).get("draft_n_accepted", 0) for r in cases for k in ("fw", "line"))
    c_err = sum(1 for r in conc for x in r["runs"] if "error" in x)
    c_tokens = [sum(x["timings"].get("predicted_n", 0) for x in r["runs"] if "timings" in x) for r in conc]
    c_wall = [r["wall_ms"] for r in conc]
    c_rate = [x["timings"].get("predicted_per_second", 0) for r in conc for x in r["runs"] if "timings" in x]
    return {
        "cases": len(cases), "errors": len(errors),
        "warm prefill ms p50/mean": (round(q(warm_ms, .5)), round(st.mean(warm_ms))),
        "warm tokens mean": round(st.mean(warm_n)),
        "warm tok/s (sum)": round(sum(warm_n) / (sum(warm_ms) / 1000)),
        "fw prefill ms p50/mean": (round(q(fw_prompt, .5)), round(st.mean(fw_prompt))),
        "fw prefill tokens mean": round(st.mean(fw_pn)),
        "fw first token ms p50": round(q(fw_tok, .5)),
        "fw first sentence ms p50/p90/mean": (round(q(fw_sent, .5)), round(q(fw_sent, .9)), round(st.mean(fw_sent))),
        "line decode tok/s mean": round(st.mean(line_rate), 1),
        "line tokens mean": round(st.mean(line_n), 1),
        "line first sentence ms p50": round(q(line_sent, .5)),
        "line total ms p50/mean": (round(q(line_total, .5)), round(st.mean(line_total))),
        "draft accepted": f"{da}/{dn} = {da / dn:.0%}" if dn else "-",
        "concurrent wall ms mean": round(st.mean(c_wall)) if c_wall else None,
        "concurrent agg tok/s": round(sum(c_tokens) / (sum(c_wall) / 1000)) if c_wall else None,
        "concurrent per-stream tok/s mean": round(st.mean(c_rate), 1) if c_rate else None,
        "concurrent errors": c_err,
        "fw samples": [r["fw"]["text"][:60] for r in cases[:4]],
    }


d = Path(sys.argv[1])
for label in ("base", "mtp3", "mtp2", "mtp4", "base2"):
    p = d / f"res-{label}.jsonl"
    if p.exists():
        print(f"== {label}")
        for k, v in summary(p).items():
            print(f"  {k}: {v}")
        vram = d / f"vram-{label}.txt"
        if vram.exists():
            print("  vram:", vram.read_text().split())
