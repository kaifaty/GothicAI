#!/usr/bin/env python3
"""Select Python test files without importing project code. E2E selection comes later."""

from __future__ import annotations

import argparse
import ast
from collections import defaultdict, deque
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import subprocess
import sys
import tempfile
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
PYTEST = ("pytest", "-q", "-rs", "--timeout=120", "--timeout-method=signal")
# The pip interface scopes the CPU index to torch; uv run's --index affects all packages.
GROUPS = tuple(Group(p, p, ("uv", "run", "--locked", "--with", "pytest-timeout", *PYTEST),
                     platform="macos-15" if p == "shorts" else "ubuntu-24.04")
               for p in LOCKED_PROJECTS) + (
    Group("body-service", "body-service", (
        "bash", "-c", 'uv venv --allow-existing .venv && '
        'uv pip install --python .venv/bin/python --torch-backend cpu '
        '--default-index https://pypi.org/simple "torch>=2.5,<3" pytest --editable . && '
        'exec uv run --no-project --python .venv/bin/python --with pytest-timeout '
        'pytest -q -rs --timeout=120 --timeout-method=signal "$@"', "pytest",
    )),
    # Shared sources belong to the first group; test membership is split by the import graph.
    Group("scripts", "scripts", (
        "uv", "run", "--no-project", "--with", "pytest-timeout", "--with", "pytest",
        "--with", "numpy", "--with", "pillow", "--with", "matplotlib", "--with", "websockets",
        "--with", "../launcher/core", *PYTEST,
    ), dependencies=("launcher/core", "server", "body-service")),
    Group("scripts-torch", "scripts", (
        "bash", "-c", 'uv venv --allow-existing .venv && '
        'uv pip install --python .venv/bin/python --torch-backend cpu '
        '--default-index https://pypi.org/simple "torch>=2.5,<3" pytest numpy pillow '
        'matplotlib websockets --editable ../body-service ../launcher/core && '
        'exec uv run --no-project --python .venv/bin/python --with pytest-timeout '
        'pytest -q -rs --timeout=120 --timeout-method=signal '
        '--ignore=content --ignore=meshes --ignore=fracture "$@"', "pytest",
    ), dependencies=("scripts",)),
    Group("content", "scripts/content", (
        "uv", "run", "--no-project", "--with", "pytest-timeout", "--with", "pytest", "--with", "numpy",
        "--with", "pillow", "--with", "scipy", "--with", "shapely",
        "--with", "ijson", "--with", "httpx", *PYTEST,
    )),
    Group("meshes", "scripts/meshes", (
        "uv", "run", "--no-project", "--with", "pytest-timeout", "--with", "pytest", *PYTEST,
    )),
    Group("fracture", "scripts/fracture", (
        "uv", "run", "--no-project", "--with", "pytest-timeout", "--with", "pytest",
        "--with", "numpy", "--with", "scipy", *PYTEST,
    )),
)

# Exact repository paths only: exclusions must not silently hide future tests.
EXCLUSIONS: dict[str, str] = {
    "reports/npc-body/2026-10-14-combat/guard-reader-v1/code/scripts/tests/test_body_combat_report.py":
        "Archived experiment snapshot: code and configurations as of the report date.",
    "reports/npc-body/2026-10-14-combat/guard-reader-v1/code/scripts/tests/test_body_final_evaluation.py":
        "Archived experiment snapshot: code and configurations as of the report date.",
    "reports/npc-body/2026-10-14-combat/guard-reader-v1/code/scripts/tests/test_body_stage8_final_report.py":
        "Archived experiment snapshot: code and configurations as of the report date.",
    "reports/npc-body/2026-10-14-combat/service-separation/validation/code/scripts/tests/test_body_combat_report.py":
        "Archived experiment snapshot: code and configurations as of the report date.",
    "reports/npc-body/2026-10-14-combat/service-separation/validation/code/scripts/tests/test_body_final_evaluation.py":
        "Archived experiment snapshot: code and configurations as of the report date.",
    "reports/npc-body/2026-10-14-combat/service-separation/validation/code/scripts/tests/test_body_stage8_final_report.py":
        "Archived experiment snapshot: code and configurations as of the report date.",
}


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


def discover_tests(root: Path, groups: tuple[Group, ...] | None = None, *,
                   graph: dict[str, set[str]] | None = None) -> dict[str, list[str]]:
    groups = GROUPS if groups is None else groups
    tests: dict[str, list[str]] = {g.name: [] for g in groups}
    for path in git_files(root, untracked=True):
        name = PurePosixPath(path).name
        if (name.startswith("test_") and name.endswith(".py") and path not in EXCLUSIONS
                and (root / path).is_file()):
            group = owner(path, groups)
            if group:
                tests[group.name].append(path)
    if "scripts-torch" in tests:
        if graph is None:
            graph = import_graph(root, groups, git_files(root, untracked=True), dependencies(root, groups))
        torch_paths = import_dependents(graph, ("torch", "gothic_body"))
        tests["scripts-torch"] = [p for p in tests["scripts"] if p in torch_paths]
        tests["scripts"] = [p for p in tests["scripts"] if p not in torch_paths]
    return tests


def dependencies(root: Path, groups: tuple[Group, ...]) -> dict[str, set[str]]:
    """Project edges come from uv path sources, including transitive dependencies."""
    by_directory: dict[Path, str] = {}
    for group in groups:
        by_directory.setdefault((root / group.directory).resolve(), group.name)
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


def parse_imports(source: str, filename: str) -> dict[str, list[str]]:
    """Extract context-free module names and file imports for one Git blob."""
    imported: set[str] = set()
    file_imports: list[ast.AST] = []
    assignments: list[ast.Assign] = []
    may_import_file = "spec_from_file_location" in source
    for node in ast.walk(ast.parse(source, filename=filename)):
        if may_import_file and isinstance(node, ast.Assign):
            assignments.append(node)
        elif isinstance(node, ast.Import):
            imported.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            base = "." * node.level + (node.module or "")
            if base:
                imported.add(base)
            separator = "" if base.endswith(".") else "."
            imported.update(base + separator + alias.name for alias in node.names if alias.name != "*")
        elif isinstance(node, ast.Call):
            function = node.func.attr if isinstance(node.func, ast.Attribute) else getattr(node.func, "id", "")
            if function in {"import_module", "__import__", "importorskip"} and node.args:
                argument = node.args[0]
                if isinstance(argument, ast.Constant) and isinstance(argument.value, str):
                    imported.add(argument.value)
            if function == "spec_from_file_location" and len(node.args) >= 2:
                file_imports.append(node.args[1])
    filenames: set[str] = set()
    if file_imports:
        # Build bindings only for files that actually import another file by path.
        bindings: dict[str, list[ast.AST]] = defaultdict(list)
        for assignment in assignments:
            for target in assignment.targets:
                if isinstance(target, ast.Name):
                    bindings[target.id].append(assignment.value)
        visited_names: set[str] = set()
        while file_imports:
            for node in ast.walk(file_imports.pop()):
                if isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value.endswith(".py"):
                    filenames.add(PurePosixPath(node.value).name)
                elif isinstance(node, ast.Name) and node.id not in visited_names:
                    visited_names.add(node.id)
                    file_imports.extend(bindings.get(node.id, ()))
    return {"imports": sorted(imported), "files": sorted(filenames)}


def load_import_cache(path: Path) -> dict[str, dict[str, list[str]]]:
    try:
        cache = json.loads(path.read_text())
    except (OSError, ValueError):
        return {}
    if not isinstance(cache, dict) or cache.get("version") != 1 or not isinstance(cache.get("entries"), dict):
        return {}
    return {key: entry for key, entry in cache["entries"].items()
            if isinstance(key, str) and isinstance(entry, dict)
            and all(isinstance(entry.get(field), list)
                    and all(isinstance(value, str) for value in entry[field]) for field in ("imports", "files"))}


def save_import_cache(path: Path, new_entries: dict[str, dict[str, list[str]]]) -> None:
    temporary: Path | None = None
    try:
        entries = load_import_cache(path)
        entries.update(new_entries)
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix="gothic-check-cache-",
                                         suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump({"version": 1, "entries": entries}, stream, separators=(",", ":"))
        os.replace(temporary, path)
    except OSError:
        pass  # An unavailable cache must not prevent test selection.
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass


def import_graph(root: Path, groups: tuple[Group, ...], files: list[str], deps: dict[str, set[str]], *,
                 parse_groups: set[str] | None = None) -> dict[str, set[str]]:
    """Reverse file edges and Torch package roots in each project's own package namespace."""
    modules: dict[str, dict[str, set[str]]] = {g.name: defaultdict(set) for g in groups}
    names: dict[str, str] = {}
    owners: dict[str, Group] = {}
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

    common = Path(subprocess.check_output(["git", "rev-parse", "--git-common-dir"], cwd=root, text=True).strip())
    cache_path = (root / common).resolve() / "gothic-check-cache.json"
    cache = load_import_cache(cache_path)
    staged = subprocess.check_output(["git", "ls-files", "-s", "-z"], cwd=root).decode()
    hashes = {}
    for record in staged.split("\0"):
        if record:
            metadata, path = record.split("\t", 1)
            mode, blob, stage = metadata.split()
            if stage == "0" and mode.startswith("100"):
                hashes[path] = blob
    dirty = subprocess.check_output(["git", "diff", "--name-only", "-z"], cwd=root).decode().split("\0")
    for path in dirty:
        hashes.pop(path, None)
    new_entries: dict[str, dict[str, list[str]]] = {}
    reverse: dict[str, set[str]] = defaultdict(set)
    for path in names:
        if PurePosixPath(path).name == "conftest.py":
            parent = str(PurePosixPath(path).parent) + "/"
            reverse[path].update(p for p in names if p.startswith(parent)
                                 and PurePosixPath(p).name.startswith("test_"))
    for path, group in owners.items():
        if (parse_groups is not None and group.name not in parse_groups) or not (root / path).is_file():
            continue
        blob = hashes.get(path)
        content: bytes | None = None
        if blob is None:
            content = (root / path).read_bytes()
            blob = hashlib.sha1(b"blob " + str(len(content)).encode() + b"\0" + content).hexdigest()
        entry = cache.get(blob)
        if entry is None:
            if content is None:
                content = (root / path).read_bytes()
            entry = parse_imports(content.decode("utf-8"), path)
            cache[blob] = new_entries[blob] = entry
        visible = [group.name, *sorted(deps[group.name])]
        package = names[path].split(".")
        if PurePosixPath(path).name != "__init__.py":
            package.pop()
        for target, target_group in owners.items() if entry["files"] else ():
            if target_group.name in visible and PurePosixPath(target).name in entry["files"]:
                reverse[target].add(path)
        for name in entry["imports"]:
            if name.startswith("."):
                level = len(name) - len(name.lstrip("."))
                name = ".".join([*package[:len(package) - level + 1], name.lstrip(".")]).rstrip(".")
            if not name:
                continue
            parts = name.split(".")
            if parts[0] in {"torch", "gothic_body"}:
                reverse[parts[0]].add(path)
            for length in range(1, len(parts) + 1):
                prefix = ".".join(parts[:length])
                for project in visible:
                    targets = modules[project].get(prefix, ())
                    if targets:
                        for target in targets:
                            reverse[target].add(path)
                        break
    if new_entries:
        save_import_cache(cache_path, new_entries)
    return reverse


def import_dependents(graph: dict[str, set[str]], paths: Iterable[str], *,
                      through: set[str] | None = None) -> set[str]:
    visited = set(paths)
    pending = deque(visited)
    while pending:
        for importer in graph.get(pending.popleft(), ()):
            if importer not in visited and (through is None or importer in through):
                visited.add(importer)
                pending.append(importer)
    return visited


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
                  groups: tuple[Group, ...] | None = None, level: str = "full") -> list[dict]:
    """Return runnable Python selections. Future E2E selection stays outside this function."""
    groups = GROUPS if groups is None else groups
    if level not in {"direct", "full"}:
        raise ValueError(f"Unsupported selection level: {level}")
    changed = sorted(set(changed_files))
    files = git_files(root, untracked=True)
    deps = dependencies(root, groups)
    inventory = discover_tests(root, groups, graph={})
    all_tests = {p for paths in inventory.values() for p in paths}
    texts = {p: (root / p).read_text() for p in all_tests}
    select_all = event == "workflow_dispatch" or any(
        p in {"scripts/check.py", ".github/workflows/tests.yml"} for p in changed)
    roots = {group.name for path in changed
             if path.endswith(".py") and not documentation(root, path)
             and (group := owner(path, groups)) is not None}
    parse_groups = {g.name for g in groups if g.name in roots or deps[g.name] & roots}
    # Text-only changes need no import graph unless a scripts test needs its runtime partition.
    if "scripts-torch" in inventory and (select_all or any(
        any(path in texts[p] or (not path.endswith(".py") and not documentation(root, path)
                                and PurePosixPath(path).name in texts[p]) for path in changed)
        for p in inventory["scripts"]
    ) or any(PurePosixPath(path).name in {"pyproject.toml", "uv.lock", "conftest.py"}
             and (group := owner(path, groups)) is not None
             and (group.name == "scripts" or group.name in deps["scripts"])
             for path in changed)):
        parse_groups.update({"scripts", *deps["scripts"]})
    reverse = (import_graph(root, groups, sorted(set(files) | set(changed)), deps, parse_groups=parse_groups)
               if parse_groups else {})
    tests = discover_tests(root, groups, graph=reverse)
    test_modules = all_tests | {
        path for path in files if path.endswith(".py")
        and ("tests" in PurePosixPath(path).parts or PurePosixPath(path).name == "conftest.py")
    }
    reasons: dict[str, set[str]] = defaultdict(set)
    whole_groups: dict[str, set[str]] = defaultdict(set)

    def select(paths: Iterable[str], reason: str) -> None:
        for path in paths:
            reasons[path].add(reason)

    if select_all:
        reason = "workflow_dispatch" if event == "workflow_dispatch" else "selector or workflow changed (rule 9)"
        select(all_tests, reason)
        for group in groups:
            whole_groups[group.name].add(reason)
    else:
        if event != "pull_request":
            raise ValueError(f"Unsupported event: {event}")
        for path in changed:
            references = [p for p, text in texts.items() if path in text]
            select(references, path + ": full path reference")
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
                select((p for p in all_tests if p.startswith(parent)), reason + ": fixture scope (rule 3)")
            elif group and path.endswith(".py"):
                through = test_modules if level == "direct" else None
                dependency = "direct import dependency" if level == "direct" else "import dependency"
                select(all_tests & import_dependents(reverse, [path], through=through),
                       reason + f": {dependency} (rules 2/3/5)")
            elif not path.endswith(".py"):
                candidates = [p for p in all_tests if owner(p, groups) == group] if group else all_tests
                matches = [p for p in candidates if PurePosixPath(path).name in texts.get(p, "")]
                select(matches, reason + ": file reference (rules 6/7)")
                if not matches and not references:
                    print(f"No tests reference {path}; no tests selected for this file.", file=sys.stderr)

    result = []
    for group in groups:
        selected = [p for p in tests[group.name] if p in reasons]
        if not selected:
            continue
        if level == "full" and not whole_groups[group.name] and len(selected) * 5 > len(tests[group.name]) * 3:
            reason = "more than 60% selected (rule 10)"
            select(tests[group.name], reason)
            whole_groups[group.name].add(reason)
            selected = tests[group.name]
        whole_group = "; ".join(sorted(whole_groups[group.name])) or None
        directory = root / group.directory
        # Commands use the group's working directory; paths in the plan remain repository-relative.
        # The two scripts groups share a directory, so even a whole group needs its own file list.
        shared_directory = sum(g.directory == group.directory for g in groups) > 1
        arguments = ([] if whole_group and not shared_directory else
                     [str((root / p).relative_to(directory)) for p in selected])
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
        local = subparsers.add_parser(name)
        local.add_argument("--base", default="origin/main")
        if name == "run":
            local.add_argument("--all", action="store_true", help="Run the full CI selection")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    event = args.event if args.mode == "ci-matrix" else "pull_request"
    paths = [] if event == "workflow_dispatch" else changed_paths(root, args.base, working_tree=args.mode != "ci-matrix")
    level = "full" if args.mode == "ci-matrix" or (args.mode == "run" and args.all) else "direct"
    selections = select_groups(root, paths, event=event, level=level)
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
    if args.mode == "plan":
        full = select_groups(root, paths, event=event, level="full")
        for group in full:
            detail = ("whole group: " + group["whole_group"] if group["whole_group"] else
                      f"test files: {len(group['files'])}")
            print(f"In CI: {group['name']}: {detail}", flush=True)
        if not full:
            print("In CI: no Python tests selected.")
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
