"""A/B of the quick model with and without MTP on recorded first-words requests (stdlib only).

python3 bench.py <url> <cases.jsonl> <label> <out.jsonl> [n_cases]
Per case, on slot i % 3 like the game: warm (history, 1 token) -> first words (stream, hint, t=0.6)
-> full line (history + phrase without the hint, t=0.7, up to 120 tokens). Then 4 concurrent full lines
on slots 0-3, five rounds.
"""
import json
import re
import sys
import threading
import time
import urllib.request

URL, CASES, LABEL, OUT = sys.argv[1:5]
N = int(sys.argv[5]) if len(sys.argv) > 5 else 40
FIRST = re.compile(r"[.!?…]+[\"»)]*\s")
MODEL = "gemma-4-12b"


def post(body, stream=False):
    req = urllib.request.Request(URL + "/v1/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    return urllib.request.urlopen(req, timeout=120)


def warm(messages, slot):
    t0 = time.monotonic()
    with post({"model": MODEL, "messages": messages, "max_tokens": 1, "id_slot": slot}) as r:
        d = json.load(r)
    return {"wall_ms": round((time.monotonic() - t0) * 1000, 1), "timings": d.get("timings", {})}


def stream(messages, slot, temperature, seed):
    t0 = time.monotonic()
    first = sentence = None
    text, timings = "", {}
    body = {"model": MODEL, "messages": messages, "max_tokens": 120, "temperature": temperature,
            "id_slot": slot, "stream": True, "seed": seed}
    with post(body) as r:
        for raw in r:
            line = raw.decode().strip()
            if not line.startswith("data: ") or line == "data: [DONE]":
                continue
            chunk = json.loads(line[6:])
            if chunk.get("timings"):
                timings = chunk["timings"]
            for ch in chunk.get("choices", []):
                piece = (ch.get("delta") or {}).get("content") or ""
                if piece:
                    now = time.monotonic()
                    first = first if first is not None else now - t0
                    text += piece
                    if sentence is None and (FIRST.search(text) or "\n\n" in text):
                        sentence = now - t0
    total = time.monotonic() - t0
    return {"first_token_ms": round((first or total) * 1000, 1),
            "first_sentence_ms": round((sentence or total) * 1000, 1),
            "total_ms": round(total * 1000, 1), "text": text, "timings": timings}


with open(CASES, encoding="utf-8") as f:
    cases = [json.loads(line) for line in f]
step = max(1, len(cases) // N)
picked = cases[::step][:N]
with open(OUT, "w", encoding="utf-8") as out:
    for i, c in enumerate(picked):
        slot = i % 3
        rec = {"label": LABEL, "i": i, "turn": c["turn"], "npc": c["npc"], "slot": slot}
        try:
            rec["warm"] = warm(c["warm"], slot)
            rec["fw"] = stream(c["full"], slot, 0.6, 1000 + i)
            rec["line"] = stream(c["warm"] + [c["full"][-1]], slot, 0.7, 2000 + i)
        except Exception as err:  # noqa: BLE001 - the bench records failures and goes on
            rec["error"] = repr(err)
        out.write(json.dumps(rec, ensure_ascii=False) + "\n")
        out.flush()
        print(LABEL, i, rec.get("error") or (rec["fw"]["first_sentence_ms"], rec["line"]["timings"].get("predicted_per_second")),
              flush=True)

    # concurrency: four full lines at once on slots 0-3
    for rnd in range(5):
        results = [None] * 4

        def run(k, rnd=rnd, results=results):
            c = picked[(rnd * 4 + k) % len(picked)]
            try:
                results[k] = stream(c["warm"] + [c["full"][-1]], k, 0.7, 3000 + rnd * 4 + k)
            except Exception as err:  # noqa: BLE001
                results[k] = {"error": repr(err)}

        threads = [threading.Thread(target=run, args=(k,)) for k in range(4)]
        t0 = time.monotonic()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        rec = {"label": LABEL, "concurrent": rnd, "wall_ms": round((time.monotonic() - t0) * 1000, 1), "runs": results}
        out.write(json.dumps(rec, ensure_ascii=False) + "\n")
        out.flush()
        print(LABEL, "concurrent", rnd, rec["wall_ms"], [r.get("error") or r["timings"].get("predicted_n") for r in results],
              flush=True)
