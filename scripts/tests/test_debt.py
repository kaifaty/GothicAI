"""Repository measurements and issue reconciliation without GitHub writes."""

import importlib.util
import json
import os
import re
import subprocess
import sys
import textwrap
from copy import deepcopy
from datetime import date
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gothic_debt", ROOT / "scripts/debt.py")
debt = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = debt
SPEC.loader.exec_module(debt)


@pytest.fixture
def config():
    return debt.load_config(ROOT / "scripts/debt.toml")


def finding(path="module.py", *, kind="hotspot", score=400_000, state="open", level="normal"):
    metrics = {
        "hotspot": {"lines": 4_000, "commits": score // 4_000},
        "doc-now": {"bytes": score},
        "doc-file": {"bytes": score},
        "ruff": {"violations": score},
        "orphan-scripts": {"files": ["scripts/lonely.py"], "count": score},
    }[kind]
    return {
        "key": "orphan-scripts" if kind == "orphan-scripts" else f"{kind}:{path}",
        "kind": kind, "path": path, "state": state, "level": level,
        "score": score, "metrics": metrics, "title": f"Долг: {path}",
    }


def issue(config, value, *, number=1, state="open", reason=None, automatic=True):
    labels = ["долг", "долг-авто"] if automatic else ["долг"]
    return {
        "number": number, "title": "A human edited this title",
        "body": debt.issue_body(value, config), "state": state,
        "state_reason": reason, "labels": [{"name": name} for name in labels],
    }


class FakeClient:
    def __init__(self, issues=(), *, labels=("долг", "долг-авто"), incomplete=False):
        self.endpoint = "repos/fixture/repository"
        self.issues = deepcopy(list(issues))
        self.labels = [{"name": name} for name in labels]
        self.writes = []
        self.incomplete = incomplete

    def list_issues(self):
        yield from deepcopy(self.issues)
        if self.incomplete:
            raise RuntimeError("incomplete issue listing")

    def list_labels(self):
        return deepcopy(self.labels)

    def create_label(self, name, color, description):
        self.writes.append(("label", name))
        self.labels.append({"name": name})

    def create_issue(self, title, body, labels):
        self.writes.append(("create", title))
        created = {
            "number": max((item["number"] for item in self.issues), default=0) + 1,
            "title": title, "body": body, "state": "open", "state_reason": None,
            "labels": [{"name": name} for name in labels],
        }
        self.issues.append(created)
        return deepcopy(created)

    def update_issue(self, number, **fields):
        self.writes.append(("update", number, fields))
        current = next(item for item in self.issues if item["number"] == number)
        current.update(fields)
        return deepcopy(current)

    def comment(self, number, body):
        self.writes.append(("comment", number, body))


def actions(plan):
    return [item["action"] for item in plan]


def test_create_labels_and_issue_then_repeat_without_writes(config):
    value = finding()
    client = FakeClient(labels=())
    plan = debt.sync([value], config, client, apply=True)
    assert actions(plan).count("create") == 1
    assert [call[:2] for call in client.writes if call[0] == "label"] == [
        ("label", "долг"), ("label", "долг-авто"),
    ]
    assert len(client.issues) == 1
    assert "<!-- debt-key: hotspot:module.py -->" in client.issues[0]["body"]
    for heading in ("### Процент", "### Как погасить"):
        assert f"{heading}\n\n_Заполняет еженедельный разбор._" in client.issues[0]["body"]
    assert {item["name"] for item in client.issues[0]["labels"]} == {"долг", "долг-авто"}
    client.writes.clear()
    debt.sync([value], config, client, apply=True)
    assert client.writes == []
    config["hotspot"]["open_score"] = 350_000
    debt.sync([value], config, client, apply=True)
    assert [call[0] for call in client.writes] == ["update"]
    assert "350,000" in client.issues[0]["body"]
    client.writes.clear()
    debt.sync([value], config, client, apply=True)
    assert client.writes == []


def test_create_limit_orders_heavy_priority_ratio_and_key(config):
    assert {kind: config[kind]["priority"] for kind in debt.TITLES} == {
        "hotspot": 1, "doc-now": 1, "doc-file": 1, "ruff": 2, "orphan-scripts": 3,
    }
    values = [finding(f"normal-{index}.py", score=300_000 + index * 100_000)
              for index in range(6)]
    values += [finding("heavy.py", score=1_200_000, level="heavy"),
               finding("manual.md", kind="doc-file", score=1_000_000, level="heavy"),
               finding("heavy", kind="ruff", score=100, level="heavy"),
               finding("normal-z.py", score=800_000),
               finding("large.md", kind="doc-now", score=102_400),
               finding("server", kind="ruff", score=96),
               finding(kind="orphan-scripts", score=10)]
    client = FakeClient()
    plan = debt.sync(values, config, client, apply=True)
    created = [item["key"] for item in plan if item["action"] == "create"]
    assert created == ["doc-file:manual.md", "hotspot:heavy.py", "ruff:heavy",
                       "hotspot:normal-5.py", "hotspot:normal-z.py"]
    ordered = [item["key"] for item in plan]
    assert ordered.index("doc-now:large.md") < ordered.index("ruff:server")
    assert ordered.index("ruff:server") < ordered.index("orphan-scripts")
    assert actions(plan).count("defer") == 8
    assert len([call for call in client.writes if call[0] == "create"]) == 5
    client.writes.clear()
    next_batch = debt.sync(values, config, client, apply=True)
    assert actions(next_batch).count("create") == 5
    assert [call[0] for call in client.writes] == ["create"] * 5


def test_update_preserves_human_text_and_comments_only_on_heavy_transition(config):
    previous = finding(score=500_000)
    current = finding(score=1_200_000, level="heavy")
    stored = issue(config, previous)
    prefix = "Процент: 17 часов в неделю.\r\nТочный текст: ё, пробелы  \n"
    suffix = "\r\nКак погасить: разделить по контрактам.\n<!-- human: keep -->\n"
    stored["body"] = prefix + stored["body"] + suffix
    client = FakeClient([stored])
    plan = debt.sync([current], config, client, apply=True)
    assert "update" in actions(plan) and "comment" in actions(plan)
    assert client.issues[0]["body"].startswith(prefix)
    assert client.issues[0]["body"].endswith(suffix)
    assert client.issues[0]["title"] == stored["title"]
    def human_text(body):
        body = re.sub(r"<!-- debt-metrics:start -->.*?<!-- debt-metrics:end -->", "METRICS",
                      body, flags=re.DOTALL)
        return re.sub(r"<!-- debt-data: .*? -->", "DATA", body)

    assert human_text(client.issues[0]["body"]) == human_text(stored["body"])
    assert client.issues[0]["body"].count("_Заполняет еженедельный разбор._") == 2
    client.writes.clear()
    debt.sync([current], config, client, apply=True)
    assert client.writes == []
    higher = finding(score=1_300_000, level="heavy")
    debt.sync([higher], config, client, apply=True)
    assert not [call for call in client.writes if call[0] == "comment"]


@pytest.mark.parametrize("reason", ["completed", "not_planned"])
def test_closed_issue_reopens_only_at_required_threshold(config, reason):
    previous = finding(score=400_000)
    client = FakeClient([issue(config, previous, state="closed", reason=reason)])
    waiting = finding(score=200_000, state="hold") if reason == "completed" else finding(score=599_999)
    debt.sync([waiting], config, client, apply=True)
    assert client.writes == []
    assert client.issues[0]["body"] == debt.issue_body(previous, config)
    current = finding(score=400_000 if reason == "completed" else 600_000)
    plan = debt.sync([current], config, client, apply=True)
    assert "reopen" in actions(plan) and "comment" in actions(plan)
    assert client.issues[0]["state"] == "open"
    client.writes.clear()
    debt.sync([current], config, client, apply=True)
    assert client.writes == []


def test_hold_updates_existing_but_never_creates_and_missing_closes(config):
    held = finding(score=200_000, state="hold")
    client = FakeClient([issue(config, finding())])
    plan = debt.sync([held, finding("new.py", score=200_000, state="hold")], config,
                     client, apply=True)
    assert "update" in actions(plan)
    assert "create" not in actions(plan) and "close" not in actions(plan)
    assert len(client.issues) == 1
    client.writes.clear()
    plan = debt.sync([], config, client, apply=True)
    assert "comment" in actions(plan) and "close" in actions(plan)
    assert client.issues[0]["state"] == "closed"
    assert client.issues[0]["state_reason"] == "completed"
    client.writes.clear()
    debt.sync([], config, client, apply=True)
    assert client.writes == []


def test_manual_issue_claims_key_without_changes_and_pull_request_does_not(config):
    value = finding()
    manual = issue(config, value, automatic=False)
    pull_request = issue(config, finding("other.py"), number=2)
    pull_request["pull_request"] = {"url": "https://api.github.com/pulls/2"}
    client = FakeClient([manual, pull_request])
    plan = debt.sync([finding(score=1_500_000, level="heavy"), finding("other.py")],
                     config, client, apply=True)
    assert [item["key"] for item in plan if item["action"] == "create"] == ["hotspot:other.py"]
    assert client.issues[0] == manual
    assert not [call for call in client.writes if len(call) > 1 and call[1] == 1]


@pytest.mark.parametrize("extra_automatic", [False, True])
def test_manual_owner_accepts_duplicate_keys_without_writes(config, monkeypatch, extra_automatic):
    value = finding()
    manual = issue(config, value, automatic=False)
    manual["body"] = "<!-- debt-key: another-key -->\n" + manual["body"]
    automatic = [issue(config, value, number=2, state="closed", reason="duplicate")]
    if extra_automatic:
        automatic += [issue(config, value, number=3), issue(config, value, number=4)]
    client = FakeClient([*automatic, manual])
    plan = debt.sync([value], config, client, apply=True)
    assert all(row["action"] == "skip" for row in plan)
    assert value["key"] in {row["key"] for row in plan}
    assert client.writes == []
    monkeypatch.setenv("GITHUB_REPOSITORY", "fixture/repository")
    monkeypatch.setattr(debt, "scan", lambda *args: [value])
    monkeypatch.setattr(debt, "GhClient", lambda repository: client)
    assert debt.main(["sync"]) == 0
    assert client.writes == []


def test_duplicate_automatic_issues_are_ignored_but_multiple_owners_fail_before_writes(config):
    value = finding()
    duplicate = issue(config, value, number=2, state="closed", reason="duplicate")
    client = FakeClient([duplicate, issue(config, value)])
    assert actions(debt.sync([value], config, client, apply=True)) == ["skip"]
    assert client.writes == []
    client = FakeClient([duplicate])
    assert actions(debt.sync([value], config, client, apply=True)) == ["create"]
    client = FakeClient([duplicate, issue(config, value), issue(config, value, number=3)], labels=())
    with pytest.raises(ValueError, match="Multiple issues with debt key"):
        debt.sync([value, finding("new.py")], config, client, apply=True)
    assert client.writes == []


@pytest.mark.parametrize("incomplete", [False, True])
def test_dry_run_and_incomplete_read_never_write(config, incomplete):
    client = FakeClient([issue(config, finding())], labels=(), incomplete=incomplete)
    if incomplete:
        with pytest.raises(RuntimeError, match="incomplete"):
            debt.sync([finding("new.py")], config, client, apply=True)
    else:
        plan = debt.sync([finding("new.py")], config, client)
        assert {"create", "close"} <= set(actions(plan))
    assert client.writes == []


@pytest.mark.parametrize("failure", ["request", "incomplete"])
def test_paginated_github_failure_finishes_before_any_write(config, monkeypatch, failure):
    client = debt.GhClient("fixture/repository")
    calls = []

    def api(method, path, data=None, *, include=False):
        calls.append((method, path))
        assert method == "GET" and include
        if "&page=1" in path:
            return ('HTTP/2.0 200 OK\nLink: <https://api.github.com/issues?page=2>; rel="next"\n\n'
                    + json.dumps([issue(config, finding())]))
        if failure == "request":
            raise RuntimeError("second page failed")
        return 'HTTP/2.0 200 OK\n\n{"message":"incomplete listing"}'

    monkeypatch.setattr(client, "api", api)
    with pytest.raises((RuntimeError, ValueError)):
        debt.sync([finding("new.py")], config, client, apply=True)
    assert len(calls) == 2
    assert all(method == "GET" for method, _ in calls)


def test_plan_is_printed_and_duplicated_as_summary_table(tmp_path, monkeypatch, capsys):
    summary = tmp_path / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    plan = [{"action": "create", "key": "hotspot:module.py", "reason": "выше порога"},
            {"action": "defer", "key": "doc-file:large.md", "reason": "лимит 5"}]
    debt.print_plan(plan)
    assert "создать | hotspot:module.py | выше порога" in capsys.readouterr().out
    assert summary.read_text() == (
        "| Действие | Ключ | Причина |\n|---|---|---|\n"
        "| создать | hotspot:module.py | выше порога |\n"
        "| отложить | doc-file:large.md | лимит 5 |\n"
    )


def git(root, *args, date=None):
    env = os.environ.copy()
    if date:
        env.update(GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True, env=env).stdout.strip()


def commit(root, message, date):
    git(root, "add", ".")
    git(root, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
        "commit", "-qm", message, date=date)


@pytest.fixture
def repository(tmp_path, monkeypatch):
    files = {
        "src/hot.py": "pass\n" * 20,
        "src/hold.py": "pass\n" * 12,
        "src/cold.py": "pass\n" * 6,
        "docs/live.md": "## Как работает сейчас\n" + "я" * 80 + "\n### Детали\nтекст\n## План\n" + "z" * 500,
        "docs/hold.md": "## Как работает сейчас\n" + "я" * 35 + "\n# История\n" + "z" * 500,
        "docs/map.md": "я" * 120,
        "docs/list.md": "я" * 60,
        "reports/ignored.md": "## Как работает сейчас\n" + "я" * 500,
        "OpenGothic/lib/vendor.py": "pass\n" * 100,
        "scripts/lonely.py": "pass\n",
        "scripts/used.sh": "exit 0\n",
        "README.md": "used.sh\n",
        "app/pyproject.toml": "[tool.ruff]\nline-length = 100\n",
        "app/app.py": "import os\n",
    }
    for name, text in files.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    git(tmp_path, "init", "-q", "-b", "main")
    commit(tmp_path, "Old sources", "2026-01-01T12:00:00+00:00")
    for day, names in [(15, ["src/hot.py", "src/hold.py", "src/cold.py"]),
                       (20, ["src/hot.py"]), (25, ["src/hot.py"])]:
        for name in names:
            path = tmp_path / name
            path.write_text(path.read_text().replace("pass", f"# {day}", 1))
        commit(tmp_path, f"Edit on day {day}", f"2026-02-{day}T12:00:00+00:00")
    (tmp_path / "README.md").write_text("used.sh\nHEAD clock\n")
    commit(tmp_path, "HEAD date", "2026-03-01T12:00:00+00:00")
    bin_dir = tmp_path / "fake-bin"
    bin_dir.mkdir()
    executable = bin_dir / "uvx"
    executable.write_text(
        f"#!{sys.executable}\nimport json, os, sys\n"
        "assert sys.argv[1] == 'ruff@0.17.0', sys.argv\n"
        "assert os.path.isfile('pyproject.toml')\n"
        "assert open('app.py').read() == 'import os\\n'\n"
        "if os.environ.get('FAIL_RUFF'):\n"
        "    print('ruff failed', file=sys.stderr)\n    sys.exit(2)\n"
        "print(json.dumps([{'code': 'F401'}] * 10))\nsys.exit(1)\n"
    )
    executable.chmod(0o755)
    monkeypatch.setenv("PATH", str(bin_dir) + os.pathsep + os.environ["PATH"])
    return tmp_path


def test_scan_all_kinds_head_content_utf8_and_hysteresis(repository, config):
    config["hotspot"].update(open_lines=15, close_lines=10, open_score=30,
                             close_score=10, heavy_score=50)
    config["doc-now"].update(open_score=150, close_score=100, heavy_score=250)
    config["doc-file"].update(paths=["docs/map.md", "docs/list.md"], open_score=200,
                              close_score=100, heavy_score=400)
    values = debt.scan(repository, config)
    by_key = {value["key"]: value for value in values}
    assert {value["kind"] for value in values} == {
        "hotspot", "doc-now", "doc-file", "ruff", "orphan-scripts",
    }
    assert by_key["hotspot:src/hot.py"]["score"] == 60
    assert by_key["hotspot:src/hot.py"]["metrics"] == {"lines": 20, "commits": 3}
    assert by_key["hotspot:src/hot.py"]["level"] == "heavy"
    assert by_key["hotspot:src/hold.py"]["state"] == "hold"
    assert "hotspot:src/cold.py" not in by_key
    assert by_key["doc-now:docs/live.md"]["state"] == "open"
    assert by_key["doc-now:docs/hold.md"]["state"] == "hold"
    assert by_key["doc-file:docs/map.md"]["score"] == 240
    assert by_key["doc-file:docs/list.md"]["state"] == "hold"
    assert by_key["ruff:app"]["score"] == 10
    assert by_key["orphan-scripts"]["metrics"]["files"] == ["scripts/lonely.py"]
    assert not any("ignored" in value["path"] or "vendor" in value["path"] for value in values)
    live = (repository / "docs/live.md").read_text().split("## План")[0]
    assert by_key["doc-now:docs/live.md"]["score"] == len(live.encode("utf-8"))
    for name in ["src/hot.py", "docs/live.md", "docs/map.md", "app/app.py", "app/pyproject.toml"]:
        (repository / name).write_text("dirty\n")
    assert debt.scan(repository, config) == values


def test_scan_failure_exits_nonzero_instead_of_printing_empty_findings(repository, monkeypatch):
    monkeypatch.setenv("FAIL_RUFF", "1")
    result = subprocess.run([sys.executable, str(ROOT / "scripts/debt.py"), "scan"],
                            cwd=repository, capture_output=True, text=True, check=False)
    assert result.returncode != 0
    assert result.stdout.strip() != "[]"
    assert "ruff" in result.stderr.lower()


@pytest.mark.parametrize("fence", ["```", "~~~"])
def test_doc_now_ignores_headings_inside_fenced_code(repository, config, fence):
    section = ("## Как работает сейчас\n" + "я" * 80 + "\n" + fence + "bash\n"
               "# comment\n## another comment\necho ready\n" + fence + "\nAfter the example.\n")
    (repository / "docs/live.md").write_text(section + "## План\nNot in the section.\n")
    commit(repository, "Document fenced example", "2026-03-02T12:00:00+00:00")
    config["doc-now"].update(open_score=150, close_score=100, heavy_score=500)
    values = {value["key"]: value for value in debt.scan(repository, config)}
    assert values["doc-now:docs/live.md"]["score"] == len(section.encode("utf-8"))


def test_scan_uses_head_clock_and_counts_changed_nonmerge_commits(repository, config):
    git(repository, "switch", "-qc", "side", "HEAD^")
    hot = repository / "src/hot.py"
    hot.write_text(hot.read_text().replace("# 15", "# side"))
    commit(repository, "Side edit", "2026-02-26T12:00:00+00:00")
    git(repository, "switch", "-q", "main")
    git(repository, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
        "merge", "--no-ff", "-qm", "Merge fixture", "side", date="2026-03-02T12:00:00+00:00")
    config["hotspot"].update(open_lines=15, close_lines=10, open_score=30,
                             close_score=10, heavy_score=50)
    current = {value["key"]: value for value in debt.scan(repository, config)}
    # January's initial commit is outside the HEAD-relative window. The merge itself is excluded.
    assert current["hotspot:src/hot.py"]["metrics"] == {"lines": 20, "commits": 4}
    assert current["hotspot:src/hot.py"]["score"] == 80


def test_workflow_serializes_each_ref_without_cancelling_running_jobs():
    workflow = (ROOT / ".github/workflows/debt.yml").read_text(encoding="utf-8")
    concurrency = re.search(r"^concurrency:\n((?:[ \t]+[^\n]*\n)+)", workflow, re.MULTILINE)
    assert concurrency is not None
    settings = dict(line.strip().split(": ", 1) for line in concurrency[1].splitlines())
    assert settings["group"] == "debt-${{ github.ref }}"
    assert settings["cancel-in-progress"] == "false"


@pytest.fixture
def ratchet_repository(repository, config, monkeypatch):
    config["hotspot"].update(open_lines=15, close_lines=10, open_score=20, close_score=10)
    config["doc-now"].update(open_score=150, close_score=100)
    target = repository / "OpenGothic/common/testbench.cpp"
    target.parent.mkdir(parents=True)
    (repository / "src/hot.py").rename(target)
    commit(repository, "Move hot fixture", "2026-03-02T12:00:00+00:00")
    git(repository, "switch", "-qc", "branch")
    monkeypatch.chdir(repository)
    monkeypatch.setattr(debt, "load_config", lambda _: config)
    monkeypatch.setenv("FAIL_RUFF", "1")
    monkeypatch.setattr(debt, "orphan_scripts", lambda *args: pytest.fail("check searched for orphans"))
    return repository


@pytest.mark.parametrize("today, level, exit_code", [
    (date(2026, 11, 18), "warning", 0),
    (date(2026, 11, 19), "error", 1),
    (date(2026, 11, 20), "error", 1),
])
def test_check_hotspot_annotation_exit_and_summary(ratchet_repository, config, monkeypatch, capsys,
                                                 today, level, exit_code):
    repository = ratchet_repository
    path = "OpenGothic/common/testbench.cpp"
    (repository / path).write_text("pass\n" * 22)
    # HEAD is outside the baseline's activity window; baseline findings must still apply.
    commit(repository, "Grow hot file", "2026-05-01T12:00:00+00:00")
    summary = repository / "summary.md"
    summary.write_text("Earlier step.\n")
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    check = debt.check
    monkeypatch.setattr(debt, "check", lambda root, cfg, base: check(root, cfg, base, today=today))
    assert debt.main(["check", "--base", "main"]) == exit_code
    message = ("Строк в горячем файле: 20 → 22 (+2). Новый код — в новые файлы; "
               "рост, уже влитый в main, не считается.")
    if level == "warning":
        message += " Пока это предупреждение; с 2026-11-19 такой рост не пройдёт проверку."
    assert capsys.readouterr().out == (
        f"::{level} file={path},title=Техдолг%3A горячий файл растёт::{message}\n")
    mode = ("Пока это предупреждение; с 2026-11-19 — проверка." if level == "warning" else
            "Проверка не пройдена: горячие файлы и большие разделы «Как работает сейчас» не должны расти.")
    assert summary.read_text() == (
        "Earlier step.\n### Храповик техдолга\n\n| Файл | Мера | Было | Стало | Рост |\n"
        "|---|---|---|---|---|\n| OpenGothic/common/testbench.cpp | строк | 20 | 22 | +2 |\n\n"
        + mode + "\n")


@pytest.mark.parametrize("change", ["unrelated", "decrease", "delete", "hold", "cold", "new", "dirty"])
def test_check_ignores_non_growth_and_files_not_open_on_base(ratchet_repository, config, monkeypatch,
                                                          capsys, change):
    repository = ratchet_repository
    hot = repository / "OpenGothic/common/testbench.cpp"
    if change == "delete":
        hot.unlink()
    else:
        target, text = {
            "unrelated": (repository / "README.md", "Different file.\n"),
            "decrease": (hot, "pass\n" * 19),
            "hold": (repository / "src/hold.py", "pass\n" * 30),
            "cold": (repository / "src/cold.py", "pass\n" * 30),
            "new": (repository / "src/new.py", "pass\n" * 30),
            "dirty": (hot, "pass\n" * 30),
        }[change]
        target.write_text(text)
    if change != "dirty":
        commit(repository, "Change fixture", "2026-05-01T12:00:00+00:00")
    summary = repository / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    assert debt.check(repository, config, "main", today=date(2026, 11, 19)) == 0
    expected = "Храповик техдолга: горячие файлы и большие разделы «Как работает сейчас» не выросли.\n"
    assert capsys.readouterr().out == expected
    assert summary.read_text() == expected


@pytest.mark.parametrize("merge", [False, True])
def test_check_excludes_growth_on_base_after_branching(ratchet_repository, config, capsys, merge):
    repository = ratchet_repository
    git(repository, "switch", "-q", "main")
    (repository / "OpenGothic/common/testbench.cpp").write_text("pass\n" * 25)
    commit(repository, "Growth on main", "2026-03-03T12:00:00+00:00")
    git(repository, "switch", "-q", "branch")
    (repository / "README.md").write_text("Branch change.\n")
    commit(repository, "Unrelated branch change", "2026-03-04T12:00:00+00:00")
    if merge:
        git(repository, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
            "merge", "--no-ff", "-qm", "Merge main", "main", date="2026-03-05T12:00:00+00:00")
    assert debt.check(repository, config, "main", today=date(2026, 11, 19)) == 0
    assert "не выросли" in capsys.readouterr().out


@pytest.mark.parametrize("change", ["large", "at_threshold", "small", "outside", "removed"])
def test_check_document_section_growth(ratchet_repository, config, monkeypatch, capsys, change):
    repository = ratchet_repository
    path = "docs/hold.md" if change == "small" else "docs/live.md"
    document = repository / path
    before = debt.now_section_bytes(document.read_bytes())
    if change == "at_threshold":
        config["doc-now"]["open_score"] = before
    if change == "removed":
        document.write_text("# History\nNo current section.\n")
    elif change == "outside":
        document.write_text(document.read_text() + "\nOutside the section.\n")
    else:
        document.write_text(document.read_text().replace("## Как работает сейчас\n",
                                                       "## Как работает сейчас\nещё\n"))
    commit(repository, "Edit document", "2026-05-01T12:00:00+00:00")
    summary = repository / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    assert debt.check(repository, config, "main", today=date(2026, 11, 18)) == 0
    output = capsys.readouterr().out
    if change in {"large", "at_threshold"}:
        after = before + len("ещё\n".encode())
        assert output == (
            f"::warning file={path},title=Техдолг%3A раздел «Как работает сейчас» растёт::"
            f"Байт в разделе «Как работает сейчас»: {before} → {after} (+{after - before}); "
            f"порог открытия — {config['doc-now']['open_score']}. "
            "Подробности — в отдельный документ или в «План и журнал». "
            "Пока это предупреждение; с 2026-11-19 такой рост не пройдёт проверку.\n")
        assert f"| {path} | байт раздела | {before} | {after} | +{after - before} |" in summary.read_text()
    else:
        assert "не выросли" in output and "::warning" not in output and "::error" not in output


def test_check_escapes_workflow_command_properties(ratchet_repository, config, capsys):
    repository = ratchet_repository
    target = repository / "OpenGothic/common/test,bench:100%.cpp"
    (repository / "OpenGothic/common/testbench.cpp").rename(target)
    commit(repository, "Rename hot file", "2026-03-03T12:00:00+00:00")
    base = git(repository, "rev-parse", "HEAD")
    target.write_text(target.read_text() + "pass\n")
    commit(repository, "Grow renamed file", "2026-05-01T12:00:00+00:00")
    assert debt.check(repository, config, base, today=date(2026, 11, 18)) == 0
    assert capsys.readouterr().out.startswith(
        "::warning file=OpenGothic/common/test%2Cbench%3A100%25.cpp,")
    assert debt.workflow_escape("%\r\n:,", property_value=True) == "%25%0D%0A%3A%2C"
    assert debt.workflow_escape("%\r\n:,") == "%25%0D%0A:,"


def test_check_requires_base_and_reports_git_failure(ratchet_repository, capsys):
    with pytest.raises(SystemExit) as error:
        debt.main(["check"])
    assert error.value.code == 2
    capsys.readouterr()
    assert debt.main(["check", "--base", "missing-revision"]) == 1
    output = capsys.readouterr()
    assert output.out == "" and output.err.startswith("debt: ")


def test_ratchet_workflow_checks_every_pr_against_base_sha():
    workflow = (ROOT / ".github/workflows/debt-ratchet.yml").read_text(encoding="utf-8")
    assert "name: Technical debt ratchet\n" in workflow
    assert "on: pull_request\n" in workflow and not re.search(r"\bpaths(?:-ignore)?:", workflow)
    assert 'git show "$BASE_SHA:scripts/debt.py"' in workflow
    assert 'python3 "$tool/debt.py" check --base "$BASE_SHA"' in workflow
    assert "BASE_SHA: ${{ github.event.pull_request.base.sha }}" in workflow
    assert "ref: ${{ github.event.pull_request.head.sha }}" in workflow
    assert "persist-credentials: false" in workflow and "fetch-depth: 0" in workflow
    assert 'python-version: "3.12"' in workflow
    assert "contents: read" in workflow and "timeout-minutes: 5" in workflow
    assert "group: debt-ratchet-${{ github.event.pull_request.number }}" in workflow
    assert "cancel-in-progress: true" in workflow
    tests = (ROOT / ".github/workflows/tests.yml").read_text(encoding="utf-8")
    for action in ("checkout", "setup-python"):
        pinned = re.search(rf"actions/{action}@[a-f0-9]{{40}}", tests)[0]
        assert pinned in workflow


OLD_DEBT = (
    "import argparse\n"
    "parser = argparse.ArgumentParser()\n"
    "parser.add_subparsers(dest='command', required=True).add_parser('scan')\n"
    "parser.parse_args()\n"
)
RATCHET_CONFIG = """\
exclude = ["OpenGothic/lib", "reports", "research_notes"]

[ratchet]
enforce_from = 2099-01-01

[hotspot]
extensions = [".py", ".cpp"]
window_days = 30
open_lines = 15
close_lines = 10
open_score = 20
close_score = 10

[doc-now]
open_score = 150
close_score = 100
"""


def run_ratchet_step(repository, base):
    workflow = (ROOT / ".github/workflows/debt-ratchet.yml").read_text(encoding="utf-8")
    step = re.search(r"^ +- name: Check debt growth\n(?: +.*\n)*? +run: \|\n((?:(?: {10}.*)?\n)+)",
                     workflow, re.MULTILINE)
    env = {**os.environ, "BASE_SHA": base}
    env.pop("GITHUB_STEP_SUMMARY", None)
    # GitHub's default shell for `run`.
    return subprocess.run(["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c", textwrap.dedent(step[1])],
                          cwd=repository, env=env, capture_output=True, text=True)


@pytest.mark.parametrize("base_tool", ["ratchet", "before-ratchet", "missing"])
def test_ratchet_step_measures_old_branch_with_base_tool(repository, base_tool):
    hot = repository / "OpenGothic/common/testbench.cpp"
    hot.parent.mkdir(parents=True)
    (repository / "src/hot.py").rename(hot)
    (repository / "scripts/debt.py").write_text(OLD_DEBT)
    (repository / "scripts/debt.toml").write_text(RATCHET_CONFIG)
    commit(repository, "Debt tool before the ratchet", "2026-03-02T12:00:00+00:00")
    # The branch forks before the ratchet lands: its own debt.py has no `check`.
    git(repository, "switch", "-qc", "branch")
    hot.write_text("pass\n" * 22)
    commit(repository, "Grow hot file", "2026-03-04T12:00:00+00:00")
    git(repository, "switch", "-q", "main")
    if base_tool == "ratchet":
        (repository / "scripts/debt.py").write_text((ROOT / "scripts/debt.py").read_text(encoding="utf-8"))
        commit(repository, "Ratchet lands", "2026-03-03T12:00:00+00:00")
    elif base_tool == "missing":
        (repository / "scripts/debt.py").unlink()
        commit(repository, "Remove debt tool", "2026-03-03T12:00:00+00:00")
    base = git(repository, "rev-parse", "main")
    git(repository, "switch", "-q", "branch")
    result = run_ratchet_step(repository, base)
    assert result.returncode == 0, result.stderr
    assert "invalid choice" not in result.stderr
    if base_tool == "ratchet":
        assert result.stdout == (
            "::warning file=OpenGothic/common/testbench.cpp,title=Техдолг%3A горячий файл растёт::"
            "Строк в горячем файле: 20 → 22 (+2). Новый код — в новые файлы; рост, уже влитый в main, "
            "не считается. Пока это предупреждение; с 2099-01-01 такой рост не пройдёт проверку.\n")
    else:
        assert result.stdout == ("::warning title=Храповик техдолга::На базе PR в scripts/debt.py "
                                 "нет команды check; храповик пропущен.\n")


def audit_finding(*, symbol="handle_request", severity="high", **fields):
    return {
        "match": "new", "kind": "workaround", "path": "src/hot.py", "symbol": symbol,
        "severity": severity, "title": f"Обход в {symbol}", "what": "Особый случай в обработчике.",
        "evidence": [{"path": "src/hot.py", "start": 2, "end": 4, "note": "Повторная проверка."}],
        "interest": "Три PR меняли одну проверку.", "fix": "Убрать обход; проверить контракт.",
        "size": "M", "prs": [791, 792], **fields,
    }


def audit(repository, findings=(), *, slice="A", annotations=(), bugs=()):
    return {
        "slice": slice, "base": git(repository, "rev-parse", "HEAD^"),
        "head": git(repository, "rev-parse", "HEAD"), "findings": list(findings),
        "annotations": list(annotations), "bugs": list(bugs),
    }


def test_ingest_config_has_audit_kinds_visual_exclusions_and_limit(config):
    assert set(config["codex"]["kinds"]) == {
        "duplicate-logic", "workaround", "stale-flag", "dead-code", "layer-breach",
        "untested-logic", "fragile-test", "doc-drift",
    }
    assert set(config["codex"]["exclude"]) == {
        "OpenGothic/shader", "OpenGothic/common/graphics", "OpenGothic/common/ui",
        "scripts/content", "scripts/meshes", "scripts/hd", "site", "launcher/app",
    }
    assert config["codex"]["max_new"] == 3


@pytest.mark.parametrize("change", [
    {"kind": "style"}, {"severity": "low"}, {"size": "XL"}, {"match": "#abc"},
    {"evidence": []}, {"evidence": [{"path": "src/hot.py", "start": 1, "end": 1,
                                   "note": "Repeated."}] * 5},
    {"path": "OpenGothic/common/graphics/render.cpp"},
    {"evidence": [{"path": "scripts/content/mesh.py", "start": 1, "end": 1, "note": "Excluded."}]},
    {"evidence": [{"path": "src/missing.py", "start": 1, "end": 1, "note": "Missing."}]},
    {"evidence": [{"path": "src/hot.py", "start": 0, "end": 1, "note": "Invalid."}]},
    {"evidence": [{"path": "src/hot.py", "start": 4, "end": 2, "note": "Invalid."}]},
    {"evidence": [{"path": "src/hot.py", "start": 1, "end": 21, "note": "Invalid."}]},
    {"evidence": [{"path": "src/hot.py", "start": 1, "end": 2}]},
])
def test_ingest_rejects_invalid_finding_with_reason(repository, config, change):
    client = FakeClient()
    value = audit_finding(**change)
    plan = debt.ingest(repository, [audit(repository, [value])], config, client, apply=True)
    assert actions(plan) == ["reject"]
    assert "A1" in plan[0]["key"] and plan[0]["reason"]
    assert client.writes == []


def test_ingest_rejects_missing_required_field(repository, config):
    value = audit_finding()
    del value["fix"]
    client = FakeClient()
    plan = debt.ingest(repository, [audit(repository, [value])], config, client, apply=True)
    assert actions(plan) == ["reject"] and "fix" in plan[0]["reason"]
    assert client.writes == []


def test_ingest_uses_audit_head_for_blobs_and_line_bounds(repository, config):
    document = audit(repository, [audit_finding()])
    (repository / "src/hot.py").unlink()
    (repository / "src/new.py").write_text("Only present after the audit.\n")
    commit(repository, "Move beyond audit head", "2026-03-02T12:00:00+00:00")
    document["findings"] += [audit_finding(symbol="new_file", evidence=[{
        "path": "src/new.py", "start": 1, "end": 1, "note": "Too new.",
    }])]
    client = FakeClient()
    plan = debt.ingest(repository, [document], config, client, apply=True)
    assert actions(plan).count("create") == 1 and actions(plan).count("reject") == 1
    assert document["head"] in client.issues[0]["body"]
    document["head"] = "f" * 40
    client = FakeClient(labels=())
    with pytest.raises((ValueError, subprocess.CalledProcessError)):
        debt.ingest(repository, [document], config, client, apply=True)
    assert client.writes == []


@pytest.mark.parametrize("state,reason", [("open", None), ("closed", "completed"),
                                         ("closed", "not_planned")])
def test_ingest_existing_key_in_any_state_is_skipped(repository, config, state, reason):
    value = audit_finding()
    stored = issue(config, finding(), number=44, state=state, reason=reason, automatic=False)
    stored["body"] = "<!-- debt-key: codex:workaround:src/hot.py#handle_request -->"
    client = FakeClient([stored])
    plan = debt.ingest(repository, [audit(repository, [value])], config, client, apply=True)
    assert actions(plan) == ["skip"] and "#44" in plan[0]["reason"]
    assert client.writes == []


def test_ingest_manual_owner_wins_and_closed_automatic_duplicate_is_ignored(repository, config):
    value = audit_finding()
    key = "<!-- debt-key: codex:workaround:src/hot.py#handle_request -->"
    duplicate = issue(config, finding(), number=1, state="closed", reason="duplicate")
    duplicate["body"] = key
    manual = issue(config, finding(), number=44, automatic=False)
    manual["body"] = key
    automatic = issue(config, finding(), number=2)
    automatic["body"] = key
    client = FakeClient([duplicate, automatic, manual])
    plan = debt.ingest(repository, [audit(repository, [value])], config, client, apply=True)
    assert actions(plan) == ["skip"] and "#44" in plan[0]["reason"]
    assert client.writes == []
    client = FakeClient([duplicate])
    assert actions(debt.ingest(repository, [audit(repository, [value])], config, client)) == ["create"]
    assert client.writes == []


def test_ingest_matches_known_issue_rejects_unknown_and_skips_run_duplicate(repository, config):
    document = audit(repository, [audit_finding(match="#44"), audit_finding(symbol="missing", match="#999"),
                                  audit_finding(symbol="first"), audit_finding(symbol="first")])
    client = FakeClient([issue(config, finding(), number=44, state="closed", reason="completed")])
    plan = debt.ingest(repository, [document], config, client, apply=True)
    assert actions(plan).count("create") == 1 and actions(plan).count("reject") == 1
    skipped = [row for row in plan if row["action"] == "skip"]
    assert any("сопоставлено с #44" in row["reason"] for row in skipped)
    assert any("дубль в прогоне" in row["reason"] for row in skipped)
    assert len([call for call in client.writes if call[0] == "create"]) == 1


def test_ingest_limit_orders_severity_then_files_and_only_bypasses_limit(repository, config):
    documents = [
        audit(repository, [audit_finding(symbol="a1", severity="medium"),
                           audit_finding(symbol="a2"), audit_finding(symbol="a3")]),
        audit(repository, [audit_finding(symbol="b1"), audit_finding(symbol="b2", severity="medium"),
                           audit_finding(symbol="b3", severity="medium")], slice="B"),
    ]
    client = FakeClient()
    plan = debt.ingest(repository, documents, config, client, apply=True)
    created = [row["key"] for row in plan if row["action"] == "create"]
    assert len(created) == 3 and all(identifier in key for identifier, key in zip(["A2", "A3", "B1"], created))
    assert actions(plan).count("defer") == 3
    client = FakeClient()
    plan = debt.ingest(repository, documents, config, client, only={"A1", "A2", "A3", "B2"}, apply=True)
    created = [row["key"] for row in plan if row["action"] == "create"]
    assert len(created) == 4
    assert not any("B1" in key or "B3" in key for key in created)
    client = FakeClient(labels=())
    with pytest.raises(ValueError, match="B99"):
        debt.ingest(repository, documents, config, client, only={"A1", "B99"}, apply=True)
    assert client.writes == []


def test_ingest_creates_codex_label_body_and_sync_keeps_it_manual(repository, config):
    value = audit_finding()
    document = audit(repository, [value])
    client = FakeClient()
    plan = debt.ingest(repository, [document], config, client, apply=True)
    assert actions(plan) == ["create"]
    assert [call for call in client.writes if call[0] == "label"] == [("label", "долг-codex")]
    created = client.issues[0]
    assert created["title"] == f"Долг: {value['title']}"
    assert {label["name"] for label in created["labels"]} == {"долг", "долг-codex"}
    body = created["body"]
    assert body.startswith("<!-- debt-key: codex:workaround:src/hot.py#handle_request -->")
    assert "debt-data" not in body and "debt-metrics" not in body and "### Замеры" not in body
    headings = ["### Что", "### Где", "### Процент", "### Как погасить"]
    assert [body.index(heading) for heading in headings] == sorted(body.index(heading) for heading in headings)
    for text in [value["what"], value["interest"], value["fix"], value["evidence"][0]["note"],
                 f"https://github.com/fixture/repository/blob/{document['head']}/src/hot.py#L2-L4",
                 "Вид: workaround", "тяжесть: high", "размер: M", "PR недели: #791, #792",
                 f"аудит: срез A, `{document['base']}`…`{document['head']}`"]:
        assert text in body
    client.writes.clear()
    assert actions(debt.ingest(repository, [document], config, client, apply=True)) == ["skip"]
    assert client.writes == []
    assert actions(debt.sync([], config, client, apply=True)) == ["skip"]
    assert client.writes == []


def test_ingest_annotations_replace_only_target_placeholders_preserving_rest(repository, config):
    stored = issue(config, finding(), number=793)
    placeholder = "_Заполняет еженедельный разбор._"
    prefix = f"Outside placeholder: {placeholder}\r\nKeep  spaces  \n"
    suffix = f"\r\n### Human appendix\r\n{placeholder}\r\n"
    original = prefix + stored["body"].replace("\n", "\r\n") + suffix
    stored["body"] = original
    annotation = {"issue": 793, "interest": "Четыре правки в неделю.", "fix": "Разделить обработчики.", "size": "L"}
    document = audit(repository, annotations=[annotation])
    client = FakeClient([stored])
    plan = debt.ingest(repository, [document], config, client, apply=True)
    assert actions(plan) == ["update"]
    assert [call[0] for call in client.writes] == ["update"]
    expected = original.replace("### Процент\r\n\r\n" + placeholder,
                                "### Процент\r\n\r\n" + annotation["interest"])
    expected = expected.replace("### Как погасить\r\n\r\n" + placeholder,
                                "### Как погасить\r\n\r\n" + annotation["fix"] + "\r\nРазмер: L")
    assert client.issues[0]["body"].encode() == expected.encode()
    client.writes.clear()
    plan = debt.ingest(repository, [document], config, client, apply=True)
    assert all(row["action"] == "skip" for row in plan)
    assert client.writes == []


def test_ingest_annotation_leaves_filled_section_and_only_selects_named_issue(repository, config):
    first = issue(config, finding(), number=793)
    first["body"] = first["body"].replace("_Заполняет еженедельный разбор._", "Human interest.", 1)
    second = issue(config, finding("other.py"), number=794)
    annotations = [{"issue": number, "interest": "Audit interest.", "fix": "Audit fix.", "size": "S"}
                   for number in (793, 794)]
    client = FakeClient([first, second])
    plan = debt.ingest(repository, [audit(repository, annotations=annotations)], config, client,
                       only={"D:793"}, apply=True)
    assert any(row["action"] == "skip" and "Процент" in row["reason"] for row in plan)
    assert "Human interest." in client.issues[0]["body"]
    assert "Audit interest." not in client.issues[0]["body"]
    assert "Audit fix." in client.issues[0]["body"]
    assert client.issues[1] == second
    assert [call[0:2] for call in client.writes] == [("update", 793)]


@pytest.mark.parametrize("state,automatic,exists", [("closed", True, True),
                                                   ("open", False, True), ("open", True, False)])
def test_ingest_rejects_annotation_not_on_open_automatic_issue(repository, config, state, automatic, exists):
    stored = issue(config, finding(), number=793, state=state, automatic=automatic)
    client = FakeClient([stored] if exists else [])
    document = audit(repository, annotations=[{"issue": 793, "interest": "Cost.", "fix": "Fix.", "size": "M"}])
    plan = debt.ingest(repository, [document], config, client, apply=True)
    assert actions(plan) == ["reject"] and plan[0]["reason"]
    assert client.writes == []


def test_ingest_bugs_are_plan_errors_without_writes(repository, config, capsys, monkeypatch, tmp_path):
    client = FakeClient(labels=())
    document = audit(repository, bugs=[{"path": "src/hot.py", "start": 1, "end": 2, "what": "Обратное условие."}])
    plan = debt.ingest(repository, [document], config, client, apply=True)
    assert actions(plan) == ["error"]
    assert "Обратное условие." in plan[0]["reason"]
    assert client.writes == []
    summary = tmp_path / "ingest-summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    debt.print_plan(plan)
    assert "ошибка |" in capsys.readouterr().out
    assert "| ошибка |" in summary.read_text()


def test_ingest_dry_run_and_read_failure_never_write(repository, config, monkeypatch):
    document = audit(repository, [audit_finding()], annotations=[{
        "issue": 793, "interest": "Cost.", "fix": "Fix.", "size": "M",
    }])
    stored = issue(config, finding(), number=793)
    client = FakeClient([stored], labels=())
    plan = debt.ingest(repository, [document], config, client)
    assert {"create", "update"} <= set(actions(plan))
    assert client.writes == []
    client = FakeClient([stored], labels=(), incomplete=True)
    with pytest.raises(RuntimeError, match="incomplete"):
        debt.ingest(repository, [document], config, client, apply=True)
    assert client.writes == []
    monkeypatch.chdir(repository)
    monkeypatch.setenv("GITHUB_REPOSITORY", "fixture/repository")
    monkeypatch.setattr(debt, "GhClient", lambda repository: client)
    source = repository / "audit.json"
    source.write_text(json.dumps(document))
    assert debt.main(["ingest", str(source), "--apply"]) == 1
    assert client.writes == []


@pytest.mark.parametrize("markdown", [False, True])
def test_ingest_loads_json_and_markdown_and_cli_does_not_scan(repository, config, monkeypatch, markdown):
    document = audit(repository, [audit_finding()])
    text = json.dumps(document, ensure_ascii=False)
    if markdown:
        text = "Scout finished.\n\n```json\n" + text + "\n```\nFinal note.\n"
    source = repository / "audit.txt"
    source.write_text(text)
    assert debt.load_audit(source) == document
    client = FakeClient()
    monkeypatch.chdir(repository)
    monkeypatch.setenv("GITHUB_REPOSITORY", "fixture/repository")
    monkeypatch.setattr(debt, "GhClient", lambda repository: client)
    monkeypatch.setattr(debt, "scan", lambda *args: pytest.fail("ingest must not scan the repository"))
    assert debt.main(["ingest", str(source), "--only", "A1"]) == 0
    assert client.writes == []
    if markdown:
        source.write_text(text + "\n```json\n{}\n```\n")
        with pytest.raises(ValueError):
            debt.load_audit(source)
