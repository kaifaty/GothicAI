#!/usr/bin/env python3
"""Measure repository debt and reconcile GitHub issues (read-only unless --apply)."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import tomllib
from collections import Counter
from pathlib import Path, PurePosixPath

KEY = re.compile(r"<!-- debt-key: (.*?) -->")
DATA = re.compile(r"<!-- debt-data: (.*?) -->")
METRICS = re.compile(r"<!-- debt-metrics:start -->.*?<!-- debt-metrics:end -->", re.DOTALL)
TITLES = {
    "hotspot": "Долг: горячий файл — {}",
    "doc-now": "Долг: раздел «Как работает сейчас» — {}",
    "doc-file": "Долг: размер документа — {}",
    "ruff": "Долг: ruff — {}",
    "orphan-scripts": "Долг: скрипты без ссылок в scripts/",
}
LABELS = {
    "долг": ("D93F0B", "Технический долг"),
    "долг-авто": ("FBCA04", "Замеры технического долга из CI"),
}


def load_config(path: Path) -> dict:
    return tomllib.loads(path.read_text(encoding="utf-8"))


def git(root: Path, *args: str) -> bytes:
    return subprocess.run(["git", *args], cwd=root, check=True, capture_output=True).stdout


def head_blobs(root: Path, paths: list[str]) -> dict[str, bytes]:
    """Read selected HEAD blobs in one process, never from the working tree."""
    output = subprocess.run(
        ["git", "cat-file", "--batch"], cwd=root,
        input=b"".join(f"HEAD:{path}\n".encode() for path in paths),
        check=True, capture_output=True,
    ).stdout
    blobs = {}
    offset = 0
    for path in paths:
        end = output.index(b"\n", offset)
        header = output[offset:end].split()
        if len(header) != 3 or header[1] != b"blob":
            raise ValueError(f"Cannot read HEAD blob: {path}")
        size = int(header[2])
        offset = end + 1
        blobs[path] = output[offset:offset + size]
        offset += size + 1
    return blobs


def finding(kind: str, path: str, score: int, metrics: dict, config: dict) -> dict | None:
    thresholds = config[kind]
    if score < thresholds["close_score"]:
        return None
    opened = score >= thresholds["open_score"]
    if kind == "hotspot":
        if metrics["lines"] < thresholds["close_lines"]:
            return None
        opened = opened and metrics["lines"] >= thresholds["open_lines"]
    return {
        "key": kind if kind == "orphan-scripts" else f"{kind}:{path}",
        "kind": kind, "path": path, "state": "open" if opened else "hold",
        "level": "heavy" if score >= thresholds.get("heavy_score", float("inf")) else "normal",
        "score": score, "metrics": metrics, "title": TITLES[kind].format(path),
    }


def orphan_scripts(root: Path, paths: list[str]) -> list[str]:
    scripts = {PurePosixPath(path).name: path for path in paths
               if PurePosixPath(path).parent == PurePosixPath("scripts")
               and PurePosixPath(path).suffix in {".py", ".sh"}}
    if not scripts:
        return []
    # One grep over HEAD; a match's filename lets us exclude self-references.
    with tempfile.NamedTemporaryFile() as patterns:
        patterns.write("\n".join(scripts).encode())
        patterns.flush()
        result = subprocess.run(
            ["git", "grep", "-a", "-z", "-o", "-F", "-f", patterns.name, "HEAD", "--"],
            cwd=root, capture_output=True, check=False,
        )
    if result.returncode not in (0, 1):
        raise RuntimeError(f"git grep failed: {result.stderr.decode(errors='replace')}")
    referenced = set()
    for line in result.stdout.splitlines():
        source, match = line.split(b"\0", 1)
        source = source.decode().removeprefix("HEAD:")
        # grep chooses the longest overlapping match; shorter names count too.
        for name, path in scripts.items():
            if name.encode() in match and source != path:
                referenced.add(path)
    return sorted(set(scripts.values()) - referenced)


def scan(root: Path, config: dict) -> list[dict]:
    paths = git(root, "ls-tree", "-r", "--name-only", "-z", "HEAD").decode().split("\0")
    paths = sorted(path for path in paths if path)
    included = [path for path in paths if not any(
        path == prefix.rstrip("/") or path.startswith(prefix.rstrip("/") + "/")
        for prefix in config["exclude"])]
    code = [path for path in included if PurePosixPath(path).suffix in config["hotspot"]["extensions"]]
    documents = [path for path in included if path.endswith(".md")]
    manifests = [path for path in included if PurePosixPath(path).name == "pyproject.toml"]
    blobs = head_blobs(root, sorted(set(code + documents + manifests)))
    timestamp = int(git(root, "show", "-s", "--format=%ct", "HEAD"))
    since = timestamp - config["hotspot"]["window_days"] * 86400
    history = git(root, "log", "--no-merges", f"--since-as-filter=@{since}",
                  f"--until=@{timestamp}", "--format=", "--name-only", "-z", "--no-renames", "HEAD")
    commits = Counter(path.decode() for path in history.split(b"\0") if path)
    result = []

    def add(kind: str, path: str, score: int, metrics: dict) -> None:
        item = finding(kind, path, score, metrics, config)
        if item is not None:
            result.append(item)

    for path in code:
        lines = len(blobs[path].splitlines())
        add("hotspot", path, lines * commits[path], {"lines": lines, "commits": commits[path]})
    for path in documents:
        content = blobs[path]
        start = re.search(rb"^## " + "Как работает сейчас".encode() + rb"[ \t]*\r?$", content, re.MULTILINE)
        if start:
            end = re.search(rb"^#{1,2}[ \t]+", content[start.end():], re.MULTILINE)
            size = (start.end() + end.start() if end else len(content)) - start.start()
            add("doc-now", path, size, {"bytes": size})
        if path in config["doc-file"]["paths"]:
            add("doc-file", path, len(content), {"bytes": len(content)})
    projects = [str(PurePosixPath(path).parent) for path in manifests
                if "ruff" in tomllib.loads(blobs[path].decode()).get("tool", {})]
    if projects:
        with tempfile.TemporaryDirectory(prefix="gothic-debt-") as temporary:
            snapshot = Path(temporary)
            archive_paths = projects + ([".gitignore"] if ".gitignore" in paths else [])
            subprocess.run(["tar", "-xf", "-", "-C", temporary], check=True, capture_output=True,
                           input=git(root, "archive", "HEAD", "--", *archive_paths))
            # Ruff honors .gitignore only inside a Git repository.
            git(snapshot, "init", "-q")
            for project in projects:
                lint = subprocess.run(
                    ["uvx", f"ruff@{config['ruff']['version']}", "check", "--config", "pyproject.toml",
                     "--output-format", "json"], cwd=snapshot / project, capture_output=True, text=True,
                    check=False,
                )
                if lint.returncode not in (0, 1):
                    raise RuntimeError(f"ruff failed in {project}: {lint.stderr.strip()}")
                violations = json.loads(lint.stdout)
                if not isinstance(violations, list) or (lint.returncode == 1 and not violations):
                    raise ValueError(f"Incomplete ruff result in {project}")
                add("ruff", project, len(violations), {"violations": len(violations)})
    orphans = orphan_scripts(root, paths)
    add("orphan-scripts", "scripts/", len(orphans), {"count": len(orphans), "files": orphans})
    return sorted(result, key=lambda item: item["key"])


class GhClient:
    """Small REST adapter. Complete every paginated read before reconciliation."""

    def __init__(self, repository: str):
        self.endpoint = f"repos/{repository}"

    def api(self, method: str, path: str, data: dict | None = None, *, include=False) -> str:
        command = ["gh", "api", "--method", method, path]
        if include:
            command.append("--include")
        if data is not None:
            command += ["--input", "-"]
        return subprocess.run(command, input=json.dumps(data) if data is not None else None,
                              capture_output=True, text=True, check=True).stdout

    def pages(self, path: str):
        page = 1
        while True:
            response = self.api("GET", f"{self.endpoint}/{path}&per_page=100&page={page}", include=True)
            header, body = re.split(r"\r?\n\r?\n", response, maxsplit=1)
            items = json.loads(body)
            if not isinstance(items, list) or len(items) > 100:
                raise ValueError("Incomplete GitHub listing")
            has_next = bool(re.search(r'^link:.*rel="next"', header, re.IGNORECASE | re.MULTILINE))
            if has_next and not items:
                raise ValueError("Empty GitHub page with a next link")
            yield from items
            if not has_next and len(items) < 100:
                return
            page += 1

    def list_issues(self):
        return self.pages("issues?state=all&labels=%D0%B4%D0%BE%D0%BB%D0%B3")

    def list_labels(self):
        return self.pages("labels?")

    def create_label(self, name, color, description):
        self.api("POST", f"{self.endpoint}/labels", {"name": name, "color": color, "description": description})

    def create_issue(self, title, body, labels):
        self.api("POST", f"{self.endpoint}/issues", {"title": title, "body": body, "labels": labels})

    def update_issue(self, number, **fields):
        self.api("PATCH", f"{self.endpoint}/issues/{number}", fields)

    def comment(self, number, body):
        self.api("POST", f"{self.endpoint}/issues/{number}/comments", {"body": body})


def metrics_block(item: dict, config: dict, previous: dict | None = None) -> str:
    thresholds = config[item["kind"]]
    old = f"{previous['score']:,} → " if previous else ""
    lines = ["<!-- debt-metrics:start -->", "### Замеры", "",
             (f"Уровень: {'тяжёлый' if item['level'] == 'heavy' else 'обычный'}. "
              f"Замер: {old}{item['score']:,}.")]
    if item["kind"] == "hotspot":
        lines += [(f"Строк: {item['metrics']['lines']}; коммитов без слияний за "
                   f"{config['hotspot']['window_days']} дней до HEAD: {item['metrics']['commits']}."),
                  "Частота правок — мера процента: сколько раз приходится возвращаться к файлу."]
    elif item["kind"] in {"doc-now", "doc-file"}:
        lines += [f"Размер UTF-8: {item['score']} байт ({item['score'] / 1024:.2f} КБ)."]
    elif item["kind"] == "ruff":
        lines += [f"Нарушений ruff {config['ruff']['version']}: {item['score']}."]
    else:
        lines += [f"Скриптов без ссылок: {item['score']}.",
                  *[f"- `{path}`" for path in item["metrics"]["files"]]]
    lines += [f"Открыть: ≥ {thresholds['open_score']:,}; закрыть: < {thresholds['close_score']:,}."]
    if item["kind"] == "hotspot":
        lines += [(f"Для открытия также нужно ≥ {thresholds['open_lines']} строк; "
                   f"закрыть и при < {thresholds['close_lines']} строк.")]
    if "heavy_score" in thresholds:
        lines += [f"Тяжёлый уровень: ≥ {thresholds['heavy_score']:,}."]
    return "\n".join([*lines, "<!-- debt-metrics:end -->"])


def issue_data(item: dict, config: dict) -> dict:
    return {**item, "config": config[item["kind"]]}


def data_marker(item: dict, config: dict) -> str:
    return "<!-- debt-data: " + json.dumps(issue_data(item, config), ensure_ascii=False, sort_keys=True) + " -->"


def issue_body(item: dict, config: dict) -> str:
    return (f"<!-- debt-key: {item['key']} -->\n\n{metrics_block(item, config)}\n"
            f"{data_marker(item, config)}\n\n### Процент\n\n\n### Как погасить\n\n")


def replace_metrics(body: str, item: dict, config: dict, previous: dict | None = None) -> str:
    if len(METRICS.findall(body)) != 1 or len(DATA.findall(body)) != 1:
        raise ValueError(f"Missing or ambiguous debt markers: {item['key']}")
    updated = METRICS.sub(lambda _: metrics_block(item, config, previous), body, count=1)
    return DATA.sub(lambda _: data_marker(item, config), updated, count=1)


def sync(findings: list[dict], config: dict, client, *, apply: bool = False) -> list[dict]:
    # Materialize and validate all reads before building or applying any writes.
    issues = list(client.list_issues())
    labels = list(client.list_labels())
    if any(not isinstance(label, dict) or not isinstance(label.get("name"), str) for label in labels):
        raise ValueError("Incomplete GitHub label listing")
    by_key = {}
    seen = set()
    for issue in issues:
        if "pull_request" in issue:
            continue
        if (not isinstance(issue.get("number"), int) or issue["number"] in seen
                or issue.get("state") not in {"open", "closed"}
                or "state_reason" not in issue or not isinstance(issue.get("labels"), list)
                or "body" not in issue or not isinstance(issue["body"], (str, type(None)))):
            raise ValueError("Incomplete GitHub issue listing")
        seen.add(issue["number"])
        match = KEY.search(issue["body"] or "")
        if match:
            if match[1] in by_key:
                raise ValueError(f"Multiple issues with debt key: {match[1]}")
            by_key[match[1]] = issue
    current = {item["key"]: item for item in findings}
    plan = []
    operations = []

    def action(name: str, key: str, reason: str, method=None, *args, **kwargs):
        plan.append({"action": name, "key": key, "reason": reason})
        if method:
            operations.append((method, args, kwargs))

    for key, issue in sorted(by_key.items()):
        number = issue["number"]
        item = current.get(key)
        if not any(label.get("name") == "долг-авто" for label in issue["labels"]):
            action("skip", key, f"#{number}: issue ведётся вручную")
            continue
        body = issue["body"]
        marker = DATA.search(body)
        if not marker:
            raise ValueError(f"Missing debt data: #{number}")
        previous = json.loads(marker[1])
        if item is None:
            if issue["state"] == "open":
                action("comment", key, f"#{number}: ниже порога закрытия или файл удалён",
                       client.comment, number, "Долг ниже порога закрытия или файл удалён. Закрываю как выполненный.")
                action("close", key, f"#{number}: ниже порога закрытия",
                       client.update_issue, number, state="closed", state_reason="completed")
            else:
                action("skip", key, f"#{number}: уже закрыт, ниже порога закрытия")
            continue
        if issue["state"] == "closed":
            if item["state"] != "open":
                action("skip", key, f"#{number}: ниже порога открытия")
                continue
            if (issue["state_reason"] != "completed"
                    and item["score"] < previous["score"] * config["sync"]["reopen_factor"]):
                action("skip", key, f"#{number}: рост меньше {config['sync']['reopen_factor']} раза от закрытия")
                continue
            action("reopen", key, f"#{number}: снова выше порога открытия",
                   client.update_issue, number, state="open", state_reason="reopened")
            action("comment", key, f"#{number}: долг вернулся после закрытия",
                   client.comment, number, "Находка снова выше порога открытия; долг переоткрыт.")
        if issue_data(item, config) != previous:
            updated = replace_metrics(body, item, config, previous)
            action("update", key, f"#{number}: замеры или пороги изменились ({previous['score']} → {item['score']})",
                   client.update_issue, number, body=updated)
            if issue["state"] == "open" and previous["level"] != "heavy" and item["level"] == "heavy":
                action("comment", key, f"#{number}: переход на тяжёлый уровень",
                       client.comment, number, "Долг перешёл на тяжёлый уровень; замеры обновлены в теле issue.")
        elif issue["state"] == "open":
            action("skip", key, f"#{number}: замеры не изменились")
    candidates = sorted((item for key, item in current.items() if key not in by_key),
                        key=lambda item: (item["level"] != "heavy",
                                          -item["score"] / config[item["kind"]]["open_score"], item["key"]))
    created = 0
    for item in candidates:
        key = item["key"]
        if item["state"] != "open":
            action("skip", key, "ниже порога открытия, issue ещё нет")
        elif created >= config["sync"]["max_new"]:
            action("defer", key, f"лимит {config['sync']['max_new']} новых issues за прогон")
        else:
            created += 1
            action("create", key, f"выше порога открытия, уровень {item['level']}", client.create_issue,
                   title=item["title"], body=issue_body(item, config), labels=list(LABELS))
    if apply:
        existing = {label["name"] for label in labels}
        for name, (color, description) in LABELS.items():
            if name not in existing:
                client.create_label(name, color, description)
        for method, args, kwargs in operations:
            method(*args, **kwargs)
    return plan


ACTIONS = {"create": "создать", "update": "обновить", "comment": "комментарий", "reopen": "переоткрыть",
           "close": "закрыть", "defer": "отложить", "skip": "пропустить"}


def print_plan(plan: list[dict]) -> None:
    for row in plan:
        print(f"{ACTIONS[row['action']]} | {row['key']} | {row['reason']}")
    counts = Counter(row["action"] for row in plan)
    print(", ".join(f"{label} {counts[name]}" for name, label in ACTIONS.items() if counts[name]) or "Нет находок.")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write("| Действие | Ключ | Причина |\n|---|---|---|\n")
            for row in plan:
                cells = [ACTIONS[row["action"]], row["key"], row["reason"]]
                stream.write("| " + " | ".join(cell.replace("|", "\\|").replace("\n", " ") for cell in cells) + " |\n")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("scan")
    reconcile = commands.add_parser("sync")
    reconcile.add_argument("--apply", action="store_true", help="Write issues and labels")
    args = parser.parse_args(argv)
    try:
        root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").decode().strip())
        config = load_config(Path(__file__).with_suffix(".toml"))
        findings = scan(root, config)
        if args.command == "scan":
            print(json.dumps(findings, ensure_ascii=False, indent=2))
        else:
            repository = os.environ.get("GITHUB_REPOSITORY") or subprocess.run(
                ["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"],
                cwd=root, capture_output=True, text=True, check=True,
            ).stdout.strip()
            print_plan(sync(findings, config, GhClient(repository), apply=args.apply))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"debt: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
