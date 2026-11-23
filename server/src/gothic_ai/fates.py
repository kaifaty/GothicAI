"""Read the current save's deeds and the abandoned fates of the same playthrough."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path

from .chronicle import Chronicle, Fact
from .feat_art import DEFAULT_ROOT
from .feats import CATEGORY_ICONS
from .game import GameError, GameLink
from .memory import BranchInfo, MemoryStore
from .protocol import ChronicleCmd, ChronicleEnd, ChronicleFeat, ChronicleFork, ChronicleGet, GameMessage


def _lineage(branch: str, branches: dict[str, BranchInfo]) -> dict[str, int]:
    """The prefix of each ancestor actually inherited by this branch."""
    lineage = {}
    limit = branches[branch].head
    while branch in branches:
        lineage[branch] = limit
        parent = branches[branch]
        if parent.parent is None:
            break
        limit = min(limit, parent.parent_seq)
        branch = parent.parent
    return lineage


def _root(lineage: dict[str, int], branches: dict[str, BranchInfo]) -> str | None:
    root = next(reversed(lineage))
    return root if branches[root].parent is None else None


def _picture(root: Path, feat: Fact, filename: str) -> str:
    own = root / feat["id"] / filename
    fallback = CATEGORY_ICONS / feat["category"] / filename
    path = own if own.is_file() else fallback
    return str(path.resolve()) if path.is_file() else ""


def _feats(
    entries: list[tuple[int, float, Fact]], art_root: Path, excluded: set[str],
) -> list[tuple[int, float, ChronicleFeat]]:
    epilogues = {f["feat"]: f["text"] for _, _, f in entries if f.get("fact") == "feat_epilogue"}
    shown = {f["feat"] for _, _, f in entries if f.get("fact") == "feat_epilogue_shown"}
    return [
        (seq, created, ChronicleFeat(
            **{key: fact[key] for key in ("id", "title", "text", "category", "chapter", "day")},
            engraving=_picture(art_root, fact, "engraving.jpg"),
            icon=_picture(art_root, fact, "icon.png"),
            epilogue=epilogues.get(fact["id"], "") if fact["id"] in shown else "",
        ))
        for seq, created, fact in entries if fact.get("fact") == "feat" and fact["id"] not in excluded
    ]


def collect_fates(
    memory: MemoryStore, branch: str | None, *, art_root: Path = DEFAULT_ROOT,
) -> tuple[list[ChronicleFeat], list[ChronicleFork]]:
    """Return only this playthrough, deduplicating inherited copies of each deed."""
    if branch is None:
        return [], []
    branches = {info.id: info for info in memory.branches()}
    if branch not in branches:
        return [], []
    current = _lineage(branch, branches)
    root = _root(current, branches)
    entries = [
        (seq, created, json.loads(content)) for seq, created, content in memory.feat_facts(branch)
    ]
    fate = [feat for _, _, feat in _feats(entries, art_root, set())]
    if root is None:
        return fate, []
    kept = {feat.id for feat in fate}
    seen_ends = {
        (created, json.dumps(fact, sort_keys=True))
        for _, created, fact in entries if fact.get("fact") == "fate_end"
    }
    ends: dict[tuple[str, int], list[tuple[float, ChronicleEnd]]] = {}
    # The first copy identifies the original recording; a revealed epilogue in a later copy
    # belongs to the same alternate fate, but never reveals it in the current branch.
    others: dict[str, tuple[tuple[str, int], float, int, ChronicleFeat]] = {}
    for other in branches:
        if other == branch:
            continue
        lineage = _lineage(other, branches)
        if _root(lineage, branches) != root:
            continue
        common = next(ancestor for ancestor in lineage if ancestor in current)
        cutoff = min(current[common], lineage[common])
        # A load can precede its parent's own fork: those bytes still belong to the ancestor.
        while branches[common].parent is not None and cutoff <= branches[common].parent_seq:
            common = branches[common].parent
        key = common, cutoff
        entries = [
            (seq, created, json.loads(content)) for seq, created, content in memory.feat_facts(other)
        ]
        deeds = _feats(entries, art_root, kept)
        for seq, created, feat in deeds:
            if feat.id not in others:
                others[feat.id] = key, created, seq, feat
            elif feat.epilogue and not others[feat.id][3].epilogue:
                old_key, old_created, old_seq, old_feat = others[feat.id]
                others[feat.id] = (
                    old_key, old_created, old_seq, old_feat.model_copy(update={"epilogue": feat.epilogue}),
                )
        if any(others[feat.id][0] == key for _, _, feat in deeds):
            for _, created, fact in entries:
                if fact.get("fact") != "fate_end":
                    continue
                identity = created, json.dumps(fact, sort_keys=True)
                if identity in seen_ends:
                    continue
                seen_ends.add(identity)
                ends.setdefault(key, []).append((created, ChronicleEnd(
                    **{field: fact[field] for field in ("kind", "title", "text", "name", "chapter", "day")},
                )))
    grouped: dict[tuple[str, int], list[tuple[float, int, ChronicleFeat]]] = {}
    for key, created, seq, feat in others.values():
        grouped.setdefault(key, []).append((created, seq, feat))
    depth = {ancestor: i for i, ancestor in enumerate(reversed(current))}
    forks = []
    for (common, cutoff), feats in sorted(
        grouped.items(), key=lambda item: (depth[item[0][0]], item[0][1]), reverse=True,
    ):
        chapter, day = memory.fact_clock(common, cutoff)
        forks.append(ChronicleFork(
            chapter=chapter, day=day,
            feats=[feat for _, _, feat in sorted(feats, key=lambda item: item[:2])],
            ends=[end for _, end in sorted(ends.get((common, cutoff), []), key=lambda item: item[0])],
        ))
    return fate, forks


class Fates:
    def __init__(
        self, link: GameLink, chronicle: Chronicle, *, enabled: bool = True, art_root: Path = DEFAULT_ROOT,
    ) -> None:
        self.link, self.chronicle = link, chronicle
        self.enabled, self.art_root = enabled, art_root
        self._sending: set[asyncio.Task[None]] = set()
        link.listeners.append(self._on_message)

    def read(self, req: str = "") -> ChronicleCmd:
        fate, others = collect_fates(
            self.chronicle.memory, self.chronicle.branch, art_root=self.art_root,
        ) if self.enabled else ([], [])
        return ChronicleCmd(req=req, fate=fate, others=others)

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, ChronicleGet):
            task = asyncio.create_task(self._send(self.read(message.req)))
            self._sending.add(task)
            task.add_done_callback(self._sending.discard)

    async def _send(self, response: ChronicleCmd) -> None:
        try:
            await self.link.send(response)
        except GameError:
            pass  # The next request after reconnect reads the branch again.

    async def close(self) -> None:
        self.link.listeners.remove(self._on_message)
        for task in self._sending:
            task.cancel()
        await asyncio.gather(*self._sending, return_exceptions=True)
