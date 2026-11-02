"""Repository measurements and issue reconciliation without GitHub writes."""

import importlib.util
import json
import os
import re
import subprocess
import sys
from copy import deepcopy
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
               finding("shorts", kind="ruff", score=96),
               finding(kind="orphan-scripts", score=10)]
    client = FakeClient()
    plan = debt.sync(values, config, client, apply=True)
    created = [item["key"] for item in plan if item["action"] == "create"]
    assert created == ["doc-file:manual.md", "hotspot:heavy.py", "ruff:heavy",
                       "hotspot:normal-5.py", "hotspot:normal-z.py"]
    ordered = [item["key"] for item in plan]
    assert ordered.index("doc-now:large.md") < ordered.index("ruff:shorts")
    assert ordered.index("ruff:shorts") < ordered.index("orphan-scripts")
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
