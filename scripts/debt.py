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
from datetime import date, datetime, timezone
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


def revision_blobs(root: Path, paths: list[str], revision: str) -> dict[str, bytes]:
    """Read selected committed blobs in one process, never from the working tree."""
    output = subprocess.run(
        ["git", "cat-file", "--batch"], cwd=root,
        input=b"".join(f"{revision}:{path}\n".encode() for path in paths),
        check=True, capture_output=True,
    ).stdout
    blobs = {}
    offset = 0
    for path in paths:
        end = output.index(b"\n", offset)
        header = output[offset:end].split()
        if len(header) != 3 or header[1] != b"blob":
            raise ValueError(f"Cannot read {revision} blob: {path}")
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


def orphan_scripts(root: Path, paths: list[str], revision: str = "HEAD") -> list[str]:
    scripts = {PurePosixPath(path).name: path for path in paths
               if PurePosixPath(path).parent == PurePosixPath("scripts")
               and PurePosixPath(path).suffix in {".py", ".sh"}}
    if not scripts:
        return []
    # One grep over the revision; a match's filename lets us exclude self-references.
    with tempfile.NamedTemporaryFile() as patterns:
        patterns.write("\n".join(scripts).encode())
        patterns.flush()
        result = subprocess.run(
            ["git", "grep", "-a", "-z", "-o", "-F", "-f", patterns.name, revision, "--"],
            cwd=root, capture_output=True, check=False,
        )
    if result.returncode not in (0, 1):
        raise RuntimeError(f"git grep failed: {result.stderr.decode(errors='replace')}")
    referenced = set()
    for line in result.stdout.splitlines():
        source, match = line.split(b"\0", 1)
        source = source.decode().removeprefix(revision + ":")
        # grep chooses the longest overlapping match; shorter names count too.
        for name, path in scripts.items():
            if name.encode() in match and source != path:
                referenced.add(path)
    return sorted(set(scripts.values()) - referenced)


def now_section_bytes(content: bytes) -> int | None:
    start = None
    fence = None
    offset = 0
    for line in content.splitlines(keepends=True):
        text = line.rstrip(b"\r\n")
        delimiter = re.match(rb" {0,3}(`{3,}|~{3,})(.*)$", text)
        if fence is not None:
            if (delimiter and delimiter[1][:1] == fence[:1] and len(delimiter[1]) >= len(fence)
                    and not delimiter[2].strip(b" \t")):
                fence = None
        elif delimiter and (delimiter[1][:1] == b"~" or b"`" not in delimiter[2]):
            fence = delimiter[1]
        elif start is None:
            if re.fullmatch(rb"## " + "Как работает сейчас".encode() + rb"[ \t]*", text):
                start = offset
        elif re.match(rb" {0,3}#{1,2}(?:[ \t]+|$)", text):
            return offset - start
        offset += len(line)
    return len(content) - start if start is not None else None


def scan(root: Path, config: dict, revision: str = "HEAD", *, ratchet: bool = False) -> list[dict]:
    paths = git(root, "ls-tree", "-r", "--name-only", "-z", revision).decode().split("\0")
    paths = sorted(path for path in paths if path)
    included = [path for path in paths if not any(
        path == prefix.rstrip("/") or path.startswith(prefix.rstrip("/") + "/")
        for prefix in config["exclude"])]
    code = [path for path in included if PurePosixPath(path).suffix in config["hotspot"]["extensions"]]
    documents = [path for path in included if path.endswith(".md")]
    manifests = [path for path in included if not ratchet and PurePosixPath(path).name == "pyproject.toml"]
    blobs = revision_blobs(root, sorted(set(code + documents + manifests)), revision)
    timestamp = int(git(root, "show", "-s", "--format=%ct", revision))
    since = timestamp - config["hotspot"]["window_days"] * 86400
    history = git(root, "log", "--no-merges", f"--since-as-filter=@{since}",
                  f"--until=@{timestamp}", "--format=", "--name-only", "-z", "--no-renames", revision)
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
        size = now_section_bytes(content)
        if size is not None:
            add("doc-now", path, size, {"bytes": size})
        if not ratchet and path in config["doc-file"]["paths"]:
            add("doc-file", path, len(content), {"bytes": len(content)})
    if ratchet:
        return sorted(result, key=lambda item: item["key"])
    projects = [str(PurePosixPath(path).parent) for path in manifests
                if "ruff" in tomllib.loads(blobs[path].decode()).get("tool", {})]
    if projects:
        with tempfile.TemporaryDirectory(prefix="gothic-debt-") as temporary:
            snapshot = Path(temporary)
            archive_paths = projects + ([".gitignore"] if ".gitignore" in paths else [])
            subprocess.run(["tar", "-xf", "-", "-C", temporary], check=True, capture_output=True,
                           input=git(root, "archive", revision, "--", *archive_paths))
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
    orphans = orphan_scripts(root, paths, revision)
    add("orphan-scripts", "scripts/", len(orphans), {"count": len(orphans), "files": orphans})
    return sorted(result, key=lambda item: item["key"])


def workflow_escape(text: str, *, property_value: bool = False) -> str:
    text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    return text.replace(":", "%3A").replace(",", "%2C") if property_value else text


def check(root: Path, config: dict, base: str, *, today: date | None = None) -> int:
    merge_base = git(root, "merge-base", base, "HEAD").decode().strip()
    baseline = [item for item in scan(root, config, merge_base, ratchet=True)
                if item["state"] == "open"]
    paths = set(git(root, "ls-tree", "-r", "--name-only", "-z", "HEAD").decode().split("\0"))
    blobs = revision_blobs(root, sorted({item["path"] for item in baseline} & paths), "HEAD")
    enforce_from = config["ratchet"]["enforce_from"]
    enforced = (today if today is not None else datetime.now(timezone.utc).date()) >= enforce_from
    rows = []
    for item in baseline:
        path = item["path"]
        if path not in blobs:
            continue
        if item["kind"] == "hotspot":
            before = item["metrics"]["lines"]
            after = len(blobs[path].splitlines())
            measure = "строк"
            title = "Техдолг: горячий файл растёт"
            message = (f"Строк в горячем файле: {before} → {after} (+{after - before}). "
                       "Новый код — в новые файлы; рост, уже влитый в main, не считается.")
        else:
            before = item["metrics"]["bytes"]
            after = now_section_bytes(blobs[path]) or 0
            measure = "байт раздела"
            title = "Техдолг: раздел «Как работает сейчас» растёт"
            message = (f"Байт в разделе «Как работает сейчас»: {before} → {after} (+{after - before}); "
                       f"порог открытия — {config['doc-now']['open_score']}. "
                       "Подробности — в отдельный документ или в «План и журнал».")
        if after <= before:
            continue
        if not enforced:
            message += f" Пока это предупреждение; с {enforce_from} такой рост не пройдёт проверку."
        level = "error" if enforced else "warning"
        print(f"::{level} file={workflow_escape(path, property_value=True)},"
              f"title={workflow_escape(title, property_value=True)}::{workflow_escape(message)}")
        cells = [path, measure, str(before), str(after), f"+{after - before}"]
        rows.append("| " + " | ".join(cell.replace("|", "\\|").replace("\r", " ").replace("\n", " ")
                                     for cell in cells) + " |")
    if rows:
        mode = ("Проверка не пройдена: горячие файлы и большие разделы «Как работает сейчас» не должны расти."
                if enforced else f"Пока это предупреждение; с {enforce_from} — проверка.")
        summary = "\n".join(["### Храповик техдолга", "", "| Файл | Мера | Было | Стало | Рост |",
                             "|---|---|---|---|---|", *rows, "", mode])
    else:
        summary = "Храповик техдолга: горячие файлы и большие разделы «Как работает сейчас» не выросли."
        print(summary)
    if summary_path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary_path, "a", encoding="utf-8") as stream:
            stream.write(summary + "\n")
    return int(enforced and bool(rows))


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
            f"{data_marker(item, config)}\n\n### Процент\n\n_Заполняет еженедельный разбор._\n\n"
            "### Как погасить\n\n_Заполняет еженедельный разбор._\n")


def replace_metrics(body: str, item: dict, config: dict, previous: dict | None = None) -> str:
    if len(METRICS.findall(body)) != 1 or len(DATA.findall(body)) != 1:
        raise ValueError(f"Missing or ambiguous debt markers: {item['key']}")
    updated = METRICS.sub(lambda _: metrics_block(item, config, previous), body, count=1)
    return DATA.sub(lambda _: data_marker(item, config), updated, count=1)


def read_issues(client) -> tuple[dict, list[dict], dict]:
    # Materialize and validate all reads before building or applying any writes.
    issues = list(client.list_issues())
    labels = list(client.list_labels())
    if any(not isinstance(label, dict) or not isinstance(label.get("name"), str) for label in labels):
        raise ValueError("Incomplete GitHub label listing")
    grouped = {}
    by_number = {}
    for issue in issues:
        if "pull_request" in issue:
            continue
        if (not isinstance(issue.get("number"), int) or issue["number"] in by_number
                or issue.get("state") not in {"open", "closed"}
                or "state_reason" not in issue or not isinstance(issue.get("labels"), list)
                or "body" not in issue or not isinstance(issue["body"], (str, type(None)))):
            raise ValueError("Incomplete GitHub issue listing")
        by_number[issue["number"]] = issue
        for key in set(KEY.findall(issue["body"] or "")):
            grouped.setdefault(key, []).append(issue)
    by_key = {}
    for key, matches in grouped.items():
        manual = [issue for issue in matches
                  if not any(label.get("name") == "долг-авто" for label in issue["labels"])]
        if manual:
            by_key[key] = min(manual, key=lambda issue: issue["number"])
            continue
        owners = [issue for issue in matches
                  if not (issue["state"] == "closed" and issue["state_reason"] == "duplicate")]
        if len(owners) > 1:
            raise ValueError(f"Multiple issues with debt key: {key}")
        if owners:
            by_key[key] = owners[0]
    return by_number, labels, by_key


def sync(findings: list[dict], config: dict, client, *, apply: bool = False) -> list[dict]:
    _, labels, by_key = read_issues(client)
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
                                          config[item["kind"]]["priority"],
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


def load_audit(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        openings = re.findall(r"^ {0,3}```json[ \t]*\r?$", text, re.MULTILINE)
        blocks = re.findall(r"^ {0,3}```json[ \t]*\r?\n(.*?)^ {0,3}```[ \t]*\r?$",
                            text, re.MULTILINE | re.DOTALL)
        if len(openings) != 1 or len(blocks) != 1:
            raise ValueError(f"Expected JSON or exactly one json block: {path}") from None
        return json.loads(blocks[0])


def audit_path(path: str, config: dict) -> None:
    if (not isinstance(path, str) or not path or path.startswith("/")
            or any(part in {"", ".", ".."} for part in path.split("/"))
            or any(char in path for char in "\r\n\0")):
        raise ValueError(f"Invalid repository path: {path!r}")
    if any(path == prefix or path.startswith(prefix + "/") for prefix in config["exclude"]):
        raise ValueError(f"Excluded path: {path}")


def validate_audit_finding(root: Path, item: dict, head: str, config: dict) -> str:
    if not isinstance(item, dict):
        raise ValueError("Finding must be an object")
    for field in ("kind", "path", "symbol", "severity", "title", "what", "interest", "fix", "size", "match"):
        if not isinstance(item.get(field), str) or not item[field].strip():
            raise ValueError(f"Missing or invalid field: {field}")
    for field, allowed in (("kind", config["kinds"]), ("severity", {"high", "medium"}),
                           ("size", {"S", "M", "L"})):
        if item[field] not in allowed:
            raise ValueError(f"Invalid {field}: {item[field]}")
    if item["match"] != "new" and not re.fullmatch(r"#[1-9][0-9]*", item["match"]):
        raise ValueError(f"Invalid match: {item['match']}")
    if (not isinstance(item.get("prs"), list)
            or any(type(number) is not int or number < 1 for number in item["prs"])):
        raise ValueError("Missing or invalid field: prs")
    evidence = item.get("evidence")
    if not isinstance(evidence, list) or not 1 <= len(evidence) <= 4:
        raise ValueError("Evidence must contain 1-4 citations")
    audit_path(item["path"], config)
    for citation in evidence:
        if not isinstance(citation, dict):
            raise ValueError("Citation must be an object")
        audit_path(citation.get("path"), config)
        if not isinstance(citation.get("note"), str) or not citation["note"].strip():
            raise ValueError("Missing or invalid citation note")
        start, end = citation.get("start"), citation.get("end")
        if type(start) is not int or type(end) is not int or not 1 <= start <= end:
            raise ValueError("Invalid citation lines: require 1 <= start <= end")
        try:
            content = git(root, "cat-file", "blob", f"{head}:{citation['path']}")
        except subprocess.CalledProcessError:
            raise ValueError(f"Citation file missing at {head}: {citation['path']}") from None
        if end > len(content.splitlines()):
            raise ValueError(f"Citation lines outside file at {head}: {citation['path']}")
    return f"codex:{item['kind']}:{item['path']}#{item['symbol']}"


def codex_issue_body(item: dict, audit: dict, repository: str, key: str) -> str:
    citations = "\n".join(
        f"- [{quote['path']}:{quote['start']}-{quote['end']}]"
        f"(https://github.com/{repository}/blob/{audit['head']}/{quote['path']}"
        f"#L{quote['start']}-L{quote['end']}): {quote['note']}"
        for quote in item["evidence"])
    prs = ", ".join(f"#{number}" for number in item["prs"]) or "—"
    return (f"<!-- debt-key: {key} -->\n\n### Что\n\n{item['what']}\n\n"
            f"### Где\n\n{citations}\n\n### Процент\n\n{item['interest']}\n\n"
            f"### Как погасить\n\n{item['fix']}\n\n"
            f"Вид: {item['kind']}; тяжесть: {item['severity']}; размер: {item['size']}; "
            f"PR недели: {prs}; аудит: срез {audit['slice']}, `{audit['base']}`…`{audit['head']}`\n")


def annotate_body(body: str, item: dict) -> tuple[str, list[str]]:
    skipped = []
    placeholder = "_Заполняет еженедельный разбор._"
    for heading, field in (("Процент", "interest"), ("Как погасить", "fix")):
        section = re.search(r"^### " + heading + r"[ \t]*\r?\n(?:(?!^#{1,3}(?:[ \t]|$)).)*",
                            body, re.MULTILINE | re.DOTALL)
        if not section or placeholder not in section[0]:
            skipped.append(heading)
            continue
        text = item[field]
        if field == "fix":
            newline = "\r\n" if section[0].split("\n", 1)[0].endswith("\r") else "\n"
            text += newline + "Размер: " + item["size"]
        start = section.start() + section[0].index(placeholder)
        body = body[:start] + text + body[start + len(placeholder):]
    return body, skipped


def ingest(root: Path, audits: list[dict], config: dict, client, *, apply: bool = False,
           only: set[str] | None = None) -> list[dict]:
    prepared = []
    identifiers = set()
    for audit in audits:
        if not isinstance(audit, dict):
            raise ValueError("Audit must be an object")
        for field in ("slice", "base", "head"):
            if not isinstance(audit.get(field), str) or not audit[field].strip():
                raise ValueError(f"Missing or invalid audit field: {field}")
        for field in ("findings", "annotations", "bugs"):
            if not isinstance(audit.get(field), list):
                raise ValueError(f"Missing or invalid audit field: {field}")
        try:
            head = git(root, "rev-parse", "--verify", "--end-of-options",
                       audit["head"] + "^{commit}").decode().strip()
        except subprocess.CalledProcessError:
            raise ValueError(f"Missing audit commit: {audit['head']}") from None
        for index, item in enumerate(audit["findings"], 1):
            identifier = f"{audit['slice']}{index}"
            if identifier in identifiers:
                raise ValueError(f"Ambiguous finding ID: {identifier}")
            identifiers.add(identifier)
        identifiers.update(f"D:{item.get('issue', '?')}" for item in audit["annotations"]
                           if isinstance(item, dict))
        prepared.append((audit, head))
    if only is not None and (unknown := only - identifiers):
        raise ValueError("Unknown IDs: " + ", ".join(sorted(unknown)))

    issues, labels, by_key = read_issues(client)
    plan = []
    candidates = []
    seen = set()
    updates = {}

    def action(name, identifier, reason, key=None):
        plan.append({"action": name, "id": identifier,
                     "key": f"{identifier} {key}" if key else identifier, "reason": reason})

    for audit, head in prepared:
        for index, item in enumerate(audit["findings"], 1):
            identifier = f"{audit['slice']}{index}"
            try:
                key = validate_audit_finding(root, item, head, config["codex"])
            except ValueError as error:
                action("reject", identifier, str(error))
                continue
            if key in seen:
                action("skip", identifier, "дубль в прогоне", key)
                continue
            seen.add(key)
            if key in by_key:
                action("skip", identifier, f"#{by_key[key]['number']}: ключ уже есть", key)
            elif item["match"] != "new":
                number = int(item["match"][1:])
                if number in issues:
                    action("skip", identifier, f"сопоставлено с #{number}", key)
                else:
                    action("reject", identifier, f"Issue #{number} is not in the debt listing", key)
            elif only is not None and identifier not in only:
                action("skip", identifier, "не выбран в --only", key)
            else:
                candidates.append((identifier, key, item, audit))
        for item in audit["annotations"]:
            identifier = f"D:{item.get('issue', '?')}" if isinstance(item, dict) else "D:?"
            if (not isinstance(item, dict) or type(item.get("issue")) is not int or item["issue"] < 1
                    or any(not isinstance(item.get(field), str) or not item[field].strip()
                           for field in ("interest", "fix", "size"))
                    or item["size"] not in {"S", "M", "L"}):
                action("reject", identifier, "Missing or invalid annotation fields")
                continue
            number = item["issue"]
            issue = issues.get(number)
            if (issue is None or issue["state"] != "open"
                    or not any(label.get("name") == "долг-авто" for label in issue["labels"])):
                action("reject", identifier, "Annotation requires an open долг-авто issue")
                continue
            if only is not None and identifier not in only:
                action("skip", identifier, "не выбран в --only")
                continue
            body = updates.get(number, issue["body"] or "")
            updated, skipped = annotate_body(body, item)
            for heading in skipped:
                action("skip", identifier, f"#{number}: раздел «{heading}» уже заполнен или отсутствует")
            if updated != body:
                updates[number] = updated
                action("update", identifier, f"#{number}: заполнены заглушки разбора")
        for index, bug in enumerate(audit["bugs"], 1):
            if not isinstance(bug, dict):
                raise ValueError("Bug must be an object")
            action("error", f"{audit['slice']}:bug{index}",
                   f"{bug.get('path')}:{bug.get('start')}-{bug.get('end')}: {bug.get('what')}")

    candidates.sort(key=lambda candidate: candidate[2]["severity"] != "high")
    creations = []
    for identifier, key, item, audit in candidates:
        if only is None and len(creations) >= config["codex"]["max_new"]:
            action("defer", identifier, f"лимит {config['codex']['max_new']} новых issues за прогон", key)
        else:
            body = codex_issue_body(item, audit, client.endpoint.removeprefix("repos/"), key)
            creations.append((item["title"], body))
            action("create", identifier, f"новая находка, тяжесть {item['severity']}", key)
    if apply:
        if creations:
            existing = {label["name"] for label in labels}
            required = {"долг": LABELS["долг"], "долг-codex": ("5319E7", "Технический долг из аудита Codex")}
            for name, (color, description) in required.items():
                if name not in existing:
                    client.create_label(name, color, description)
        for title, body in creations:
            client.create_issue(title="Долг: " + title, body=body, labels=["долг", "долг-codex"])
        for number, body in updates.items():
            client.update_issue(number, body=body)
    return plan


ACTIONS = {"create": "создать", "update": "обновить", "comment": "комментарий", "reopen": "переоткрыть",
           "close": "закрыть", "defer": "отложить", "skip": "пропустить", "reject": "отбросить",
           "error": "ошибка"}


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
    ratchet = commands.add_parser("check")
    ratchet.add_argument("--base", required=True, help="Compare with the merge-base of this revision and HEAD")
    reconcile = commands.add_parser("sync")
    reconcile.add_argument("--apply", action="store_true", help="Write issues and labels")
    ingest_command = commands.add_parser("ingest")
    ingest_command.add_argument("files", nargs="+", type=Path)
    ingest_command.add_argument("--apply", action="store_true", help="Write issues and labels")
    ingest_command.add_argument("--only", help="Approved finding or annotation IDs, comma-separated")
    args = parser.parse_args(argv)
    try:
        root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").decode().strip())
        config = load_config(Path(__file__).with_suffix(".toml"))
        if args.command == "check":
            return check(root, config, args.base)
        findings = scan(root, config) if args.command != "ingest" else None
        if args.command == "scan":
            print(json.dumps(findings, ensure_ascii=False, indent=2))
        else:
            repository = os.environ.get("GITHUB_REPOSITORY") or subprocess.run(
                ["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"],
                cwd=root, capture_output=True, text=True, check=True,
            ).stdout.strip()
            client = GhClient(repository)
            if args.command == "ingest":
                only = set(args.only.split(",")) if args.only is not None else None
                print_plan(ingest(root, [load_audit(path) for path in args.files], config, client,
                                  apply=args.apply, only=only))
            else:
                print_plan(sync(findings, config, client, apply=args.apply))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"debt: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
