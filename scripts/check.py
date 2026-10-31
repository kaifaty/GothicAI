#!/usr/bin/env python3
"""Select Python test files without importing project code. E2E selection comes later."""

from __future__ import annotations

import argparse
import ast
from collections import defaultdict, deque
from dataclasses import dataclass
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import subprocess
import sys
import tomllib
from typing import Iterable


@dataclass(frozen=True)
class Group:
    name: str
    directory: str
    command: tuple[str, ...]
    timeout: int = 10
    scopes: tuple[str, ...] = ()
    dependencies: tuple[str, ...] = ()
    platform: str = "ubuntu-24.04"


LOCKED_PROJECTS = (
    "server", "asr-service", "tts-service", "launcher/core",
    "libs/chatgpt-auth", "shorts", "site", "decider-service",
)
PYTEST = ("pytest", "-q", "-rs", "-o", "faulthandler_timeout=300")
# The pip interface scopes the CPU index to torch; uv run's --index affects all packages.
GROUPS = tuple(Group(p, p, ("uv", "run", "--locked", *PYTEST),
                     platform="macos-15" if p == "shorts" else "ubuntu-24.04")
               for p in LOCKED_PROJECTS) + (
    Group("body-service", "body-service", (
        "bash", "-c", 'uv venv --allow-existing .venv && '
        'uv pip install --python .venv/bin/python --torch-backend cpu '
        '--default-index https://pypi.org/simple "torch>=2.5,<3" pytest --editable . && '
        'exec .venv/bin/pytest -q -rs -o faulthandler_timeout=300 "$@"', "pytest",
    )),
    Group("scripts", "scripts", (
        "bash", "-c", 'uv venv --allow-existing .venv && '
        'uv pip install --python .venv/bin/python --torch-backend cpu '
        '--default-index https://pypi.org/simple "torch>=2.5,<3" pytest numpy pillow '
        'matplotlib websockets --editable ../body-service ../launcher/core && '
        'exec .venv/bin/pytest -q -rs -o faulthandler_timeout=300 '
        '--ignore=content --ignore=meshes --ignore=fracture "$@"', "pytest",
    ), dependencies=("launcher/core", "server", "body-service")),
    Group("content", "scripts/content", (
        "uv", "run", "--no-project", "--with", "pytest", "--with", "numpy",
        "--with", "pillow", "--with", "scipy", "--with", "shapely",
        "--with", "ijson", "--with", "httpx", *PYTEST,
    )),
    Group("meshes", "scripts/meshes", ("uv", "run", "--no-project", "--with", "pytest", *PYTEST)),
    Group("fracture", "scripts/fracture", (
        "uv", "run", "--no-project", "--with", "pytest", "--with", "numpy", "--with", "scipy", *PYTEST,
    )),
    *(Group(name, directory, ("env", "PYTHONPATH=" + os.path.relpath("scripts", directory),
                             "uv", "run", "--no-project", "--with", "pytest", *PYTEST),
            dependencies=("scripts",))
      for name, directory in (
          ("npc-body-guard", "reports/npc-body/2026-10-14-combat/guard-reader-v1/code"),
          ("npc-body-separation", "reports/npc-body/2026-10-14-combat/service-separation/validation/code"),
      )),
)

# Exact repository paths only: exclusions must not silently hide future tests.
EXCLUSIONS: dict[str, str] = {}


def git_files(root: Path, *, untracked: bool = False) -> list[str]:
    args = ["git", "ls-files", "-z", "--cached"]
    if untracked:
        args += ["--others", "--exclude-standard"]
    return sorted(set(subprocess.check_output(args, cwd=root).decode().split("\0")) - {""})


def owner(path: str, groups: tuple[Group, ...]) -> Group | None:
    matches = [(len(scope), group) for group in groups
               for scope in group.scopes or (group.directory,)
               if path == scope or path.startswith(scope.rstrip("/") + "/") or scope == "."]
    return max(matches, key=lambda item: item[0])[1] if matches else None


def discover_tests(root: Path, groups: tuple[Group, ...] | None = None) -> dict[str, list[str]]:
    groups = GROUPS if groups is None else groups
    tests: dict[str, list[str]] = {g.name: [] for g in groups}
    for path in git_files(root, untracked=True):
        name = PurePosixPath(path).name
        if (name.startswith("test_") and name.endswith(".py") and path not in EXCLUSIONS
                and (root / path).is_file()):
            group = owner(path, groups)
            if group:
                tests[group.name].append(path)
    return tests


def dependencies(root: Path, groups: tuple[Group, ...]) -> dict[str, set[str]]:
    """Project edges come from uv path sources, including transitive dependencies."""
    by_directory = {(root / g.directory).resolve(): g.name for g in groups}
    result = {g.name: set(g.dependencies) for g in groups}
    for group in groups:
        manifest = root / group.directory / "pyproject.toml"
        if manifest.is_file():
            data = tomllib.loads(manifest.read_text())
            sources = data.get("tool", {}).get("uv", {}).get("sources", {})
            for source in sources.values():
                for entry in source if isinstance(source, list) else [source]:
                    if isinstance(entry, dict) and "path" in entry:
                        target = (manifest.parent / entry["path"]).resolve()
                        if target in by_directory:
                            result[group.name].add(by_directory[target])
    for name, values in result.items():
        pending = list(values)
        while pending:
            for dependency in result[pending.pop()]:
                if dependency not in values and dependency != name:
                    values.add(dependency)
                    pending.append(dependency)
    return result


def import_graph(root: Path, groups: tuple[Group, ...], files: list[str], deps: dict[str, set[str]]) -> dict[str, set[str]]:
    """Reverse file edges, resolved in each project's own package namespace."""
    modules: dict[str, dict[str, set[str]]] = {g.name: defaultdict(set) for g in groups}
    names: dict[str, str] = {}
    owners: dict[str, Group] = {}
    trees: dict[str, ast.AST] = {}
    for path in files:
        group = owner(path, groups)
        if not group or not path.endswith(".py"):
            continue
        owners[path] = group
        relative = PurePosixPath(path).relative_to(group.directory)
        parts = list(relative.with_suffix("").parts)
        source_layout = parts[0] == "src"
        if source_layout:
            parts.pop(0)
        if parts[-1] == "__init__":
            parts.pop()
        name = ".".join(parts)
        names[path] = name
        modules[group.name][name].add(path)
        # Standalone tools add their directory to sys.path, unlike src packages.
        if not source_layout:
            modules[group.name][PurePosixPath(path).stem].add(path)
        if (root / path).is_file():
            trees[path] = ast.parse((root / path).read_text(), filename=path)

    reverse: dict[str, set[str]] = defaultdict(set)
    for path in names:
        if PurePosixPath(path).name == "conftest.py":
            parent = str(PurePosixPath(path).parent) + "/"
            reverse[path].update(p for p in names if p.startswith(parent)
                                 and PurePosixPath(p).name.startswith("test_"))
    for path, tree in trees.items():
        group = owners[path]
        visible = [group.name, *sorted(deps[group.name])]
        package = names[path].split(".")
        if PurePosixPath(path).name != "__init__.py":
            package.pop()
        imported: set[str] = set()
        file_imports: list[ast.AST] = []
        bindings: dict[str, list[ast.AST]] = defaultdict(list)
        for node in ast.walk(tree):
            if isinstance(node, ast.Assign):
                for target in node.targets:
                    if isinstance(target, ast.Name):
                        bindings[target.id].append(node.value)
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                prefix = package[:len(package) - node.level + 1] if node.level else []
                base = ".".join([*prefix, *([node.module] if node.module else [])])
                if base:
                    imported.add(base)
                imported.update(".".join(filter(None, [base, alias.name]))
                                for alias in node.names if alias.name != "*")
            elif isinstance(node, ast.Call):
                function = node.func.attr if isinstance(node.func, ast.Attribute) else getattr(node.func, "id", "")
                if function in {"import_module", "__import__"} and node.args:
                    argument = node.args[0]
                    if isinstance(argument, ast.Constant) and isinstance(argument.value, str):
                        name = argument.value
                        if name.startswith("."):
                            level = len(name) - len(name.lstrip("."))
                            name = ".".join([*package[:len(package) - level + 1], name.lstrip(".")]).rstrip(".")
                        imported.add(name)
                if function == "spec_from_file_location" and len(node.args) >= 2:
                    file_imports.append(node.args[1])
        if file_imports:
            # Tools often store SCRIPT = HERE / 'module.py' before calling importlib.
            filenames: set[str] = set()
            visited_names: set[str] = set()
            while file_imports:
                for node in ast.walk(file_imports.pop()):
                    if isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value.endswith(".py"):
                        filenames.add(PurePosixPath(node.value).name)
                    elif isinstance(node, ast.Name) and node.id not in visited_names:
                        visited_names.add(node.id)
                        file_imports.extend(bindings.get(node.id, ()))
            for target, target_group in owners.items():
                if target_group.name in visible and PurePosixPath(target).name in filenames:
                    reverse[target].add(path)
        for name in imported:
            parts = name.split(".")
            for length in range(1, len(parts) + 1):
                prefix = ".".join(parts[:length])
                for project in visible:
                    targets = modules[project].get(prefix, ())
                    if targets:
                        for target in targets:
                            reverse[target].add(path)
                        break
    return reverse


def documentation(root: Path, path: str) -> bool:
    if path.lower().endswith((".md", ".png", ".jpg", ".svg")) or path.startswith("docs/"):
        return True
    if path.startswith("reports/"):
        if PurePosixPath(path).suffix.lower() in {".jpeg", ".webp", ".gif", ".mp4", ".mov", ".pdf", ".zip", ".npz", ".npy", ".bin"}:
            return True
        if (root / path).is_file():
            with (root / path).open("rb") as stream:
                content = stream.read(8192)
            if b"\0" in content:
                return True
            try:
                content.decode("utf-8")
            except UnicodeDecodeError:
                return True
    return False


def select_groups(root: Path, changed_files: Iterable[str], *, event: str = "pull_request",
                  groups: tuple[Group, ...] | None = None) -> list[dict]:
    """Return runnable Python selections. Future E2E selection stays outside this function."""
    groups = GROUPS if groups is None else groups
    tests = discover_tests(root, groups)
    changed = sorted(set(changed_files))
    reasons: dict[str, set[str]] = defaultdict(set)
    whole_groups: dict[str, set[str]] = defaultdict(set)

    def select(paths: Iterable[str], reason: str) -> None:
        for path in paths:
            reasons[path].add(reason)

    all_tests = {p for paths in tests.values() for p in paths}
    if event == "workflow_dispatch" or any(p in {"scripts/check.py", ".github/workflows/tests.yml"} for p in changed):
        reason = "workflow_dispatch" if event == "workflow_dispatch" else "selector or workflow changed (rule 9)"
        select(all_tests, reason)
        for group in groups:
            whole_groups[group.name].add(reason)
    else:
        if event != "pull_request":
            raise ValueError(f"Unsupported event: {event}")
        files = git_files(root, untracked=True)
        deps = dependencies(root, groups)
        reverse = import_graph(root, groups, sorted(set(files) | set(changed)), deps) if any(p.endswith(".py") for p in changed) else {}
        texts = {p: (root / p).read_text() for p in all_tests if (root / p).is_file()}
        for path in changed:
            if documentation(root, path):
                continue
            group = owner(path, groups)
            reason = f"{path}"
            if path in all_tests:
                select([path], reason + ": changed test (rule 1)")
            if group and PurePosixPath(path).name in {"pyproject.toml", "uv.lock"}:
                affected = {group.name} | {name for name, values in deps.items() if group.name in values}
                for name in affected:
                    full_reason = reason + ": project dependencies changed (rules 4/5)"
                    select(tests[name], full_reason)
                    whole_groups[name].add(full_reason)
            elif group and PurePosixPath(path).name == "conftest.py":
                parent = str(PurePosixPath(path).parent) + "/"
                select((p for p in tests[group.name] if p.startswith(parent)), reason + ": fixture scope (rule 3)")
            elif group and path.endswith(".py"):
                pending = deque([path])
                visited = {path}
                while pending:
                    for importer in reverse.get(pending.popleft(), ()):
                        if importer not in visited:
                            visited.add(importer)
                            pending.append(importer)
                select(all_tests & visited, reason + ": import dependency (rules 2/3/5)")
            else:
                candidates = tests[group.name] if group else all_tests
                matches = [p for p in candidates if path in texts.get(p, "") or PurePosixPath(path).name in texts.get(p, "")]
                select(matches, reason + ": file reference (rules 6/7)")
                if not matches:
                    print(f"No tests reference {path}; no tests selected for this file.", file=sys.stderr)

    result = []
    for group in groups:
        selected = [p for p in tests[group.name] if p in reasons]
        if not selected:
            continue
        if not whole_groups[group.name] and len(selected) * 5 > len(tests[group.name]) * 3:
            reason = "more than 60% selected (rule 10)"
            select(tests[group.name], reason)
            whole_groups[group.name].add(reason)
            selected = tests[group.name]
        whole_group = "; ".join(sorted(whole_groups[group.name])) or None
        directory = root / group.directory
        # Commands use the group's working directory; paths in the plan remain repository-relative.
        arguments = [] if whole_group else [str((root / p).relative_to(directory)) for p in selected]
        result.append({"name": group.name, "dir": group.directory,
                       "command": shlex.join([*group.command, *arguments]), "timeout": group.timeout,
                       "platform": group.platform,
                       "files": selected, "whole_group": whole_group,
                       "reasons": {} if whole_group else {p: sorted(reasons[p]) for p in selected}})
    return result


def changed_paths(root: Path, base: str = "origin/main", *, working_tree: bool = False) -> list[str]:
    merge_base = subprocess.check_output(["git", "merge-base", base, "HEAD"], cwd=root, text=True).strip()
    args = ["git", "diff", "--name-only", "-z", "--no-renames", merge_base + "...HEAD"]
    paths = set(subprocess.check_output(args, cwd=root).decode().split("\0"))
    if working_tree:
        for args in (["git", "diff", "--name-only", "-z", "--no-renames", "HEAD"],
                     ["git", "ls-files", "--others", "--exclude-standard", "-z"]):
            paths.update(subprocess.check_output(args, cwd=root).decode().split("\0"))
    return sorted(paths - {""})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="mode", required=True)
    ci = subparsers.add_parser("ci-matrix")
    ci.add_argument("--event", choices=("pull_request", "workflow_dispatch"), required=True)
    ci.add_argument("--base", default="origin/main")
    for name in ("plan", "run"):
        subparsers.add_parser(name).add_argument("--base", default="origin/main")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    event = args.event if args.mode == "ci-matrix" else "pull_request"
    paths = [] if event == "workflow_dispatch" else changed_paths(root, args.base, working_tree=args.mode != "ci-matrix")
    selections = select_groups(root, paths, event=event)
    if args.mode == "ci-matrix":
        print(json.dumps([{k: v for k, v in group.items() if k != "reasons"} for group in selections], separators=(",", ":")))
        return 0
    for group in selections:
        if group["whole_group"]:
            print(f"{group['name']}: whole group: {group['whole_group']}", flush=True)
            continue
        print(f"{group['name']}: {group['command']}", flush=True)
        for path, reasons in group["reasons"].items():
            print(f"  {path}: {'; '.join(reasons)}", flush=True)
    if not selections:
        print("No Python tests selected.")
    if args.mode == "run":
        for group in selections:
            try:
                subprocess.run(shlex.split(group["command"]), cwd=root / group["dir"],
                               check=True, timeout=group["timeout"] * 60)
            except subprocess.CalledProcessError as error:
                return error.returncode
            except subprocess.TimeoutExpired:
                print(f"{group['name']} exceeded {group['timeout']} minutes.", file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
