#!/usr/bin/env python3
"""How well `MemoryStore.search` finds an npc's old lines for a `recall{query}`, against alternatives.

The history is an export of real npc messages (private, outside git: it holds the player's own words):

    sqlite3 -json "file:$HOME/.local/share/gothic-ai/memory.sqlite3?mode=ro" \
      "SELECT npc, role, content, created FROM events WHERE kind='message'
       GROUP BY npc, created, content ORDER BY created" > messages.json

Every topic of the questions file names a regex: the lines it matches are the relevant ones. A query
hits when one of the `--limit` lines a method returns is relevant. `mixed` puts every other npc's lines
into the history too, as a stand-in for a long game.

    cd server && uv run --with snowballstemmer python ../scripts/recall_eval.py \
      --messages ~/.local/share/gothic-ai/evals/recall/messages.json \
      --queries ../reports/npc-memory/2026-11-01-recall-eval/queries.json [--vectors vectors.npz]

Embeddings are computed elsewhere (no models on the Mac), on a machine with the model:

    uv run --with sentence-transformers python recall_eval.py embed --model BAAI/bge-m3 \
      --messages messages.json --queries queries.json --out vectors.npz
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import Counter
from pathlib import Path

WORDS = re.compile(r"\w+")
# Function words of Russian dialogue: they carry no topic and match almost every line.
STOP = frozenset("""
а без бы был была были было в вам вас во вот вы да даже для до его ее её если есть еще ещё же за и из
или им их к как ко когда кто ли мне меня мной мы на над не него нет ни но ну о об он она они оно от по
под при про с со так там тебе тебя то тоже только ты тут у уже чем что чтобы эта эти это этот я мой моя
моё мое мои твой твоя твоё твое твои свой своя своё свое свои себя себе сам сама где куда тогда давай
""".split())
RRF_K = 60


def load(args: argparse.Namespace) -> tuple[list[dict], dict]:
    lines = json.loads(Path(args.messages).expanduser().read_text())
    lines.sort(key=lambda m: m["created"])
    return lines, json.loads(Path(args.queries).read_text())


def queries(spec: dict):
    for topic in spec["topics"]:
        for style, text in topic["queries"].items():
            yield topic, style, text


# --- methods: each returns line indices of a history, best first ------------------------------------

class Current:
    """The search of the game itself, through a MemoryStore in memory."""

    name = "current"

    def __init__(self, history: list[dict], top_up: bool) -> None:
        from gothic_ai.memory import MemoryStore, _stems

        self.stems, self.top_up = _stems, top_up
        self.store = MemoryStore(":memory:")
        self.store.ensure_branch("eval")
        self.seq = {self.store.append("eval", "npc", m["role"], m["content"]): i
                    for i, m in enumerate(history)}
        self.texts = [m["content"].lower() for m in history]
        self.name = "current" if top_up else "current, scored only"

    def rank(self, query: str, limit: int) -> list[int]:
        found = self.store.search("eval", "npc", query, before_seq=max(self.seq), limit=limit)
        stems = self.stems(query)
        out = [self.seq[r.seq] for r in found]
        if not self.top_up:  # what the stems found, without the newest lines that fill up to the limit
            out = [i for i in out if any(s in self.texts[i] for s in stems)]
        scores = {i: sum(1 for s in stems if s in self.texts[i]) for i in out}
        return sorted(out, key=lambda i: (-scores[i], -i))


class Bm25:
    """BM25 over Snowball stems of whole words, without function words."""

    def __init__(self, history: list[dict], stop: bool = True) -> None:
        import snowballstemmer

        self.stemmer = snowballstemmer.stemmer("russian")
        self.stop = STOP if stop else frozenset()
        self.docs = [self.tokens(m["content"]) for m in history]
        self.df = Counter(t for d in self.docs for t in set(d))
        self.avg = sum(map(len, self.docs)) / max(1, len(self.docs))
        self.tf = [Counter(d) for d in self.docs]
        self.name = "bm25 + snowball" + (" + stop" if stop else "")

    def tokens(self, text: str) -> list[str]:
        words = [w for w in WORDS.findall(text.lower().replace("ё", "е")) if w not in self.stop]
        return self.stemmer.stemWords(words)

    def scores(self, query: str) -> dict[int, float]:
        n, out = len(self.docs), {}
        for t in set(self.tokens(query)):
            df = self.df.get(t)
            if not df:
                continue
            idf = math.log(1 + (n - df + 0.5) / (df + 0.5))
            for i, tf in enumerate(self.tf):
                if f := tf.get(t):
                    norm = f * 2.2 / (f + 1.2 * (0.25 + 0.75 * len(self.docs[i]) / self.avg))
                    out[i] = out.get(i, 0.0) + idf * norm
        return out

    def rank(self, query: str, limit: int) -> list[int]:
        s = self.scores(query)
        return sorted(s, key=lambda i: (-s[i], -i))[:limit]


class Dense:
    """Cosine similarity of embeddings computed by `embed`."""

    def __init__(self, history: list[dict], vectors: dict) -> None:
        import numpy as np

        self.np = np
        index = vectors["index"]
        self.docs = np.stack([vectors["lines"][index[m["content"]]] for m in history])
        self.queries = vectors["queries"]
        self.name = f"dense {vectors['model']}"

    def similarities(self, query: str):
        return self.docs @ self.queries[query]

    def rank(self, query: str, limit: int) -> list[int]:
        return [int(i) for i in self.np.argsort(-self.similarities(query))[:limit]]


class Hybrid:
    """Reciprocal rank fusion of BM25 and the embeddings."""

    def __init__(self, bm25: Bm25, dense: Dense) -> None:
        self.bm25, self.dense = bm25, dense
        self.name = f"hybrid rrf ({dense.name.removeprefix('dense ')})"

    def rank(self, query: str, limit: int) -> list[int]:
        fused: dict[int, float] = {}
        lexical = self.bm25.rank(query, 50)
        dense = self.dense.rank(query, 50)
        for ranks in (lexical, dense):
            for r, i in enumerate(ranks):
                fused[i] = fused.get(i, 0.0) + 1 / (RRF_K + r + 1)
        return sorted(fused, key=lambda i: -fused[i])[:limit]


# --- evaluation ---------------------------------------------------------------------------------------

def histories(lines: list[dict], spec: dict, mixed: bool) -> dict[str, list[dict]]:
    if mixed:
        return {name: lines for name in spec["histories"]}
    return {name: [m for m in lines if m["npc"] in npcs] for name, npcs in spec["histories"].items()}


def evaluate(args: argparse.Namespace) -> None:
    lines, spec = load(args)
    vectors = load_vectors(args.vectors) if args.vectors else []
    styles = sorted({s for t in spec["topics"] for s in t["queries"]}, key=["same", "form", "para"].index)
    report: dict = {}
    for mixed in (False, True):
        setting = "mixed" if mixed else "own"
        for name, history in histories(lines, spec, mixed).items():
            methods = [Current(history, True), Current(history, False), Bm25(history, stop=False),
                       Bm25(history)]
            for v in vectors:
                dense = Dense(history, v)
                methods += [dense, Hybrid(methods[3], dense)]
            for topic, style, text in queries(spec):
                if topic["history"] != name:
                    continue
                pattern = re.compile(topic["relevant"], re.I)
                relevant = {i for i, m in enumerate(history) if pattern.search(m["content"])}
                if not relevant:
                    sys.exit(f"{topic['id']}: the regex matches no line")
                for method in methods:
                    ranked = method.rank(text, args.limit)
                    rank = next((r for r, i in enumerate(ranked) if i in relevant), None)
                    row = report.setdefault(setting, {}).setdefault(method.name, {}).setdefault(style, [])
                    row.append({"topic": topic["id"], "query": text, "relevant": len(relevant),
                                "rank": rank, "found": sum(1 for i in ranked if i in relevant)})
    print_report(report, styles, args.limit)
    if args.json:
        Path(args.json).write_text(json.dumps(report, ensure_ascii=False, indent=1))


def print_report(report: dict, styles: list[str], limit: int) -> None:
    for setting, methods in report.items():
        print(f"\n### {setting}: hit@{limit} (MRR)\n")
        print("| method | " + " | ".join(styles) + " | all |")
        print("|---|" + "---|" * (len(styles) + 1))
        for name, by_style in methods.items():
            cells = [cell(by_style[s]) for s in styles] + [cell([r for s in styles for r in by_style[s]])]
            print(f"| {name} | " + " | ".join(cells) + " |")
    for setting, methods in report.items():
        print(f"\n### {setting}: misses per query (rank of the first relevant line, - = none)\n")
        names = list(methods)
        print("| style | query | " + " | ".join(names) + " |")
        print("|---|---|" + "---|" * len(names))
        first = methods[names[0]]
        for style in styles:
            for k, row in enumerate(first[style]):
                ranks = [methods[n][style][k]["rank"] for n in names]
                print(f"| {style} | {row['query']} | "
                      + " | ".join("-" if r is None else str(r + 1) for r in ranks) + " |")


def cell(rows: list[dict]) -> str:
    hits = sum(1 for r in rows if r["rank"] is not None)
    mrr = sum(1 / (r["rank"] + 1) for r in rows if r["rank"] is not None) / len(rows)
    return f"{hits}/{len(rows)} ({mrr:.2f})"


# --- embeddings ---------------------------------------------------------------------------------------

PREFIXES = {"intfloat/multilingual-e5": ("query: ", "passage: ")}


def embed(args: argparse.Namespace) -> None:
    import numpy as np
    from sentence_transformers import SentenceTransformer

    lines, spec = load(args)
    model = SentenceTransformer(args.model, device=args.device)
    q_prefix, d_prefix = next((p for k, p in PREFIXES.items() if args.model.startswith(k)), ("", ""))
    texts = sorted({m["content"] for m in lines})
    texts_q = sorted({text for _, _, text in queries(spec)})
    docs = model.encode([d_prefix + t for t in texts], normalize_embeddings=True, batch_size=64)
    qs = model.encode([q_prefix + t for t in texts_q], normalize_embeddings=True)
    np.savez(args.out, model=args.model, lines=docs, line_texts=np.array(texts, dtype=object),
             queries=qs, query_texts=np.array(texts_q, dtype=object))
    print(f"{args.model}: {len(texts)} lines, {len(texts_q)} queries -> {args.out}")


def load_vectors(paths: list[str]) -> list[dict]:
    import numpy as np

    out = []
    for path in paths:
        z = np.load(path, allow_pickle=True)
        out.append({
            "model": str(z["model"]).split("/")[-1],
            "lines": z["lines"],
            "index": {t: i for i, t in enumerate(z["line_texts"])},
            "queries": {t: z["queries"][i] for i, t in enumerate(z["query_texts"])},
        })
    return out


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", default="eval", choices=["eval", "embed"])
    parser.add_argument("--messages", required=True)
    parser.add_argument("--queries", required=True)
    parser.add_argument("--limit", type=int, default=6, help="lines a recall returns (MemoryStore.search)")
    parser.add_argument("--vectors", nargs="*", default=[], help="eval: .npz files from embed")
    parser.add_argument("--json", help="eval: write the per-query results here")
    parser.add_argument("--model", help="embed: a sentence-transformers model")
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--out", help="embed: the .npz to write")
    args = parser.parse_args()
    (embed if args.command == "embed" else evaluate)(args)


if __name__ == "__main__":
    main()
