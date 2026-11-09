"""Build quick-model request bodies (prewarm + first words) from a recorded game trace.

Usage: uv run python extract.py <traces.jsonl> <out.jsonl>
Each output line: {"turn", "npc", "warm": [chat...], "full": [chat...]}; the bench adds model/params.
"""
import json
import sys
from pathlib import Path

from gothic_ai.dialogue import REACT_HINT
from gothic_ai.llm import Message
from gothic_ai.llm.openai_compat import chat_messages
from gothic_ai.replay import _quick_context
from gothic_ai.trace import read

records = read(Path(sys.argv[1]))
instructions = {r["hash"]: r["text"] for r in records if r.get("type") == "instructions"}
out = []
for r in records:
    if not (r.get("type") == "turn" and r.get("trigger", {}).get("kind") == "player" and r.get("steps")
            and not r["steps"][0].get("llm") and r.get("instructions") in instructions):
        continue
    history, situation = _quick_context(r)
    system = {"role": "system", "content": instructions[r["instructions"]]}
    request = [*history, Message("developer", situation + "\n" + REACT_HINT), Message("user", r["trigger"]["text"])]
    out.append({
        "turn": r.get("id"), "npc": r["npc"],
        "warm": [system, *chat_messages(history)],
        "full": [system, *chat_messages(request)],
    })
with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.writelines(json.dumps(o, ensure_ascii=False) + "\n" for o in out)
print(len(out), "cases;", len({o["npc"] for o in out}), "npcs")
