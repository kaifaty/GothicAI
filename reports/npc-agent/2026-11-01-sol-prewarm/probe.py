"""Does a WebSocket warm-up (`generate: false`) bring Sol's first words sooner?

Replays the first step of one real npc turn from a turn trace (instructions, the 15 offered tools, history,
situation and the hero's phrase) over a plan-token WebSocket, the way `openai_responses.py` sends it.
Each trial opens its own connection, as `open_ahead` does when the hero starts talking, and waits HOLD_S
(the hero speaking, then ASR) before the real request:
- `cold`: the whole request goes then, nothing sent before it (the first turn with an npc today);
- `warm`: right after the connection opens, a `generate: false` request with everything the npc knows
  before the phrase (instructions, tools, history); the real request then chains from it with
  `previous_response_id` and sends only the situation and the phrase.
A nonce at the start of the instructions keeps one trial's prompt out of another's cache. Pairs alternate
their order. Run from server/:

    uv run python ../reports/npc-agent/2026-11-01-sol-prewarm/probe.py check TRACE --t T
    uv run python ../reports/npc-agent/2026-11-01-sol-prewarm/probe.py run TRACE --t T --pairs 20 --out OUT
"""

from __future__ import annotations

import argparse
import asyncio
import importlib
import json
import pkgutil
import statistics
import sys
import time
import uuid
from pathlib import Path
from typing import Any

from openai import AsyncOpenAI

import gothic_ai
from gothic_ai.llm import DEFAULT_MODELS, Tool
from gothic_ai.llm.openai_responses import WS_BETA, _error_code, _tool, input_items
from gothic_ai.llm.siwc import ChatGPTAuth
from gothic_ai.trace import item_from_json

HOLD_S = 2.0  # from the connection open to the real request: the hero's phrase and its recognition
TAIL = 2  # the items a turn adds at its start: the situation and the hero's phrase
DELTAS = ("response.output_text.delta", "response.function_call_arguments.delta")
TIMING = "responsesapi.websocket_timing"  # the server's own breakdown of a response's time
METRICS = (  # of its timing_metrics, kept and summed up
    "pre_inference_ms",
    "engine_queue_max_ms",
    "engine_service_ttft_total_ms",
    "engine_iapi_ttft_total_ms",
    "engine_cached_prompt_tokens_total",
    "engine_uncached_prompt_tokens_total",
    "first_sampled_message_reasoning_tokens",
    "responses_duration_excl_engine_wait_and_sampling_ms",
    "server_timing_total_ms",
)


def all_tools() -> dict[str, Tool]:
    """Every Tool the server defines, by name (a trace keeps only the names)."""
    found: dict[str, Tool] = {}
    for info in pkgutil.walk_packages(gothic_ai.__path__, "gothic_ai."):
        if ".scenarios" in info.name:
            continue
        try:
            module = importlib.import_module(info.name)
        except Exception:
            continue
        for value in vars(module).values():
            for x in value if isinstance(value, (list, tuple)) else [value]:
                if isinstance(x, Tool):
                    found.setdefault(x.name, x)
    return found


def load_turn(path: Path, t: float) -> dict[str, Any]:
    """Instructions, tool names and the first step's whole input of the turn that started at `t`."""
    instructions: dict[str, str] = {}
    for line in path.read_text().splitlines():
        record = json.loads(line)
        if record.get("type") == "instructions":
            instructions[record["hash"]] = record["text"]
        elif record.get("type") == "turn" and abs(record["t"] - t) < 0.01:
            items = [i for i in map(item_from_json, record["steps"][0]["added"]) if i is not None]
            return {
                "npc": record["npc"],
                "instructions": instructions[record["instructions"]],
                "tools": record["tools"],
                "input": input_items(items),
            }
    raise SystemExit(f"no turn at {t} in {path}")


class Probe:
    def __init__(self, turn: dict[str, Any], model: str) -> None:
        self.turn = turn
        self.model = model
        tools = all_tools()
        self.tools = [_tool(tools[name]) for name in turn["tools"]]
        self.token = ChatGPTAuth()
        self.client = AsyncOpenAI(api_key="per-request", max_retries=0)

    def body(self, nonce: str, items: list[dict[str, Any]]) -> dict[str, Any]:
        return {
            "model": self.model,
            "instructions": f"[{nonce}]\n{self.turn['instructions']}",
            "input": items,
            "store": False,
            "stream": True,
            "reasoning": {"effort": "low"},
            "include": ["reasoning.encrypted_content"],
            "tools": self.tools,
            "prompt_cache_key": f"probe-{nonce}",
        }

    async def connect(self) -> tuple[Any, float]:
        started = time.monotonic()
        token = await self.token()
        conn = await self.client.responses.connect(
            extra_headers={"Authorization": f"Bearer {token}", "OpenAI-Beta": WS_BETA}
        ).enter()
        return conn, time.monotonic() - started

    async def send(self, conn: Any, body: dict[str, Any], *, verbose: bool = False) -> dict[str, Any]:
        """One response.create to its end: times from the send, usage, its id, or the error."""
        started = time.monotonic()
        await conn.send_raw(json.dumps({"type": "response.create", **body}))
        out: dict[str, Any] = {"events": []}
        while True:
            event = await conn.recv()
            at = round(time.monotonic() - started, 3)
            if verbose:
                print(f"  {at:6.3f} {event.type}", file=sys.stderr)
            out["events"].append(event.type)
            out.setdefault("first_event_s", at)
            if event.type == TIMING:
                metrics = getattr(event, "timing_metrics", None) or {}
                out["server"] = {k: metrics.get(k) for k in METRICS}
            if event.type in DELTAS:
                out.setdefault("first_delta_s", at)
            if event.type == "response.output_text.delta":
                out.setdefault("first_text_s", at)
                out["text"] = out.get("text", "") + event.delta
            if event.type == "error":
                out.update(total_s=at, error=_error_code(event) or str(event)[:500])
                return out
            if event.type in ("response.completed", "response.failed", "response.incomplete"):
                response = event.response
                usage = getattr(response, "usage", None)
                details = getattr(usage, "input_tokens_details", None)
                out.update(
                    total_s=at,
                    status=getattr(response, "status", event.type),
                    id=getattr(response, "id", None),
                    output_items=len(getattr(response, "output", None) or []),
                    input_tokens=getattr(usage, "input_tokens", None),
                    cached_tokens=getattr(details, "cached_tokens", None),
                    output_tokens=getattr(usage, "output_tokens", None),
                )
                if event.type != "response.completed":
                    why = getattr(response, "error", None) or getattr(response, "incomplete_details", "")
                    out["error"] = str(why)
                return out

    async def trial(self, kind: str) -> dict[str, Any]:
        nonce = uuid.uuid4().hex[:12]
        whole = self.turn["input"]
        conn, open_s = await self.connect()
        result: dict[str, Any] = {"kind": kind, "nonce": nonce, "open_s": round(open_s, 3)}
        try:
            opened = time.monotonic()
            if kind == "warm":
                warm = await self.send(conn, {**self.body(nonce, whole[:-TAIL]), "generate": False})
                result["warmup"] = {k: v for k, v in warm.items() if k != "events"}
                if "error" in warm or not warm.get("id"):
                    return result
            await asyncio.sleep(max(0.0, opened + HOLD_S - time.monotonic()))
            if kind == "warm":
                body = {**self.body(nonce, whole[-TAIL:]), "previous_response_id": warm["id"]}
            else:
                body = self.body(nonce, whole)
            result["real"] = {k: v for k, v in (await self.send(conn, body)).items() if k != "events"}
        finally:
            await conn.close()
        return result


async def check(probe: Probe) -> None:
    """A warm-up with every event printed, then the chained request: is `generate: false` taken at all?"""
    nonce = uuid.uuid4().hex[:12]
    whole = probe.turn["input"]
    conn, open_s = await probe.connect()
    print(f"connection opened in {open_s:.2f} s", file=sys.stderr)
    try:
        print("warm-up (generate: false):", file=sys.stderr)
        warm = await probe.send(conn, {**probe.body(nonce, whole[:-TAIL]), "generate": False}, verbose=True)
        print(json.dumps(warm, ensure_ascii=False))
        if "error" in warm or not warm.get("id"):
            return
        print("chained real request:", file=sys.stderr)
        body = {**probe.body(nonce, whole[-TAIL:]), "previous_response_id": warm["id"]}
        real = await probe.send(conn, body, verbose=True)
        print(json.dumps({k: v for k, v in real.items() if k != "events"}, ensure_ascii=False))
    finally:
        await conn.close()


async def run(probe: Probe, pairs: int, out: Path) -> None:
    with out.open("a") as f:
        for i in range(pairs):
            for kind in ("cold", "warm") if i % 2 == 0 else ("warm", "cold"):
                result = {"pair": i, **await probe.trial(kind)}
                f.write(json.dumps(result, ensure_ascii=False) + "\n")
                f.flush()
                real = result.get("real", {})
                print(
                    f"pair {i:2d} {kind}: first delta {real.get('first_delta_s')} s, cached "
                    f"{real.get('cached_tokens')}/{real.get('input_tokens')}, warm-up "
                    f"{result.get('warmup', {}).get('total_s')} s {real.get('error') or ''}",
                    file=sys.stderr,
                )
    summary(out)


def summary(path: Path) -> None:
    rows = [json.loads(line) for line in path.read_text().splitlines()]

    def first(row: dict[str, Any]) -> float | None:
        return row.get("real", {}).get("first_delta_s")

    for kind in ("cold", "warm"):
        values = sorted(v for r in rows if r["kind"] == kind and (v := first(r)) is not None)
        if not values:
            continue
        q = statistics.quantiles(values, n=4) if len(values) > 1 else [values[0]] * 3
        cached = [r["real"].get("cached_tokens") or 0 for r in rows if r["kind"] == kind and "real" in r]
        print(
            f"{kind}: n={len(values)} first delta median {statistics.median(values):.3f} s, "
            f"p25 {q[0]:.3f}, p75 {q[2]:.3f}, min {values[0]:.3f}, max {values[-1]:.3f}; "
            f"cached tokens median {statistics.median(cached):.0f}"
        )
        for key in METRICS:
            server = [r.get("real", {}).get("server", {}) for r in rows if r["kind"] == kind]
            got = [v for s in server if (v := s.get(key)) is not None]
            if got:
                print(f"  server {key}: median {statistics.median(got):.1f}, max {max(got):.1f}")
    warmups = [r["warmup"]["total_s"] for r in rows if "warmup" in r and "total_s" in r["warmup"]]
    if warmups:
        median = statistics.median(warmups)
        print(f"warm-up: n={len(warmups)} median {median:.3f} s, max {max(warmups):.3f} s")
    by_pair: dict[int, dict[str, float]] = {}
    for r in rows:
        if (v := first(r)) is not None:
            by_pair.setdefault(r["pair"], {})[r["kind"]] = v
    diffs = [p["cold"] - p["warm"] for p in by_pair.values() if len(p) == 2]
    if diffs:
        print(
            f"paired cold - warm: n={len(diffs)} median {statistics.median(diffs):.3f} s, "
            f"warm sooner in {sum(d > 0 for d in diffs)}/{len(diffs)}"
        )


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("mode", choices=("check", "run", "summary"))
    p.add_argument("trace", type=Path, help="a turn trace (traces/<day>.jsonl), or the results for summary")
    p.add_argument("--t", type=float, help="the start time `t` of the turn to replay")
    p.add_argument("--pairs", type=int, default=20)
    p.add_argument("--out", type=Path)
    p.add_argument("--model", default=DEFAULT_MODELS["chatgpt"])
    args = p.parse_args()
    if args.mode == "summary":
        summary(args.trace)
        return
    probe = Probe(load_turn(args.trace, args.t), args.model)
    print(
        f"{probe.turn['npc']}: {len(probe.turn['input'])} items, {len(probe.tools)} tools, {probe.model}",
        file=sys.stderr,
    )
    if args.mode == "check":
        asyncio.run(check(probe))
    else:
        asyncio.run(run(probe, args.pairs, args.out.resolve()))


if __name__ == "__main__":
    main()
