#!/usr/bin/env python3
"""Keep clips, frames and heavy files out of git: check the files a branch adds.

The repository is public and the history of `main` cannot be rewritten, so a file that lands there stays
forever (`AGENTS.md`, section «Pull request'ы и `main`»). Two rules for every file the branch's commits add
or change, including ones a later commit of the branch deletes again:
- one file up to LIMIT bytes;
- no GLB, DDS or Blender files at all: they live in the task's folder or on the shared storage.

    python3 scripts/large_files.py check [--base origin/main] [--head HEAD]
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import PurePosixPath

LIMIT = 3 * 1024 * 1024
BANNED_SUFFIXES = {".glb", ".dds", ".blend", ".blend1"}


@dataclass(frozen=True)
class Blob:
    path: str
    size: int


def file_problem(blob: Blob) -> str | None:
    """Why this file must not enter git, or None when it may."""
    suffix = PurePosixPath(blob.path).suffix.casefold()
    if suffix in BANNED_SUFFIXES:
        return f"{blob.path}: файлы {suffix} держим вне git"
    if blob.size > LIMIT:
        return f"{blob.path}: {blob.size / 1024 / 1024:.1f} МБ, больше предела {LIMIT // 1024 // 1024} МБ"
    return None


def branch_blobs(base: str, head: str) -> list[Blob]:
    """Blobs reachable from head and not from base, each with the first path git found it at."""
    objects = subprocess.run(
        ["git", "rev-list", "--objects", f"{base}..{head}"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    if not objects.strip():
        return []
    sizes = subprocess.run(
        ["git", "cat-file", "--batch-check=%(objecttype) %(objectsize) %(rest)"],
        input=objects,
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    blobs = []
    for line in sizes.splitlines():
        kind, size, path = (line.split(" ", 2) + [""])[:3]
        if kind == "blob" and path:
            blobs.append(Blob(path, int(size)))
    return blobs


def report(text: str) -> None:
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::error title=Файл вне правил git::{text}")
    else:
        print(f"error: {text}")


def check(base: str, head: str) -> int:
    blobs = branch_blobs(base, head)
    problems = [problem for blob in blobs if (problem := file_problem(blob))]
    for text in problems:
        report(text)
    if problems:
        print(
            f"\nФайлов вне правил: {len(problems)}. Ролики, полные кадры, GLB, DDS и .blend кладите в .bench-out/,"
            "\nпапку задачи или на хранилище, а в заметку пишите путь. Файл должен уйти из всех коммитов ветки:"
            "\ngit rm --cached <файл> и git commit --amend в коммите, который его добавил, затем"
            "\ngit push --force-with-lease."
        )
        return 1
    print(f"Новых файлов в ветке: {len(blobs)}, все в пределах.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("check", help="check the files the commits between base and head add")
    run.add_argument("--base", default="origin/main")
    run.add_argument("--head", default="HEAD")
    args = parser.parse_args(argv)
    return check(args.base, args.head)


if __name__ == "__main__":
    sys.exit(main())
