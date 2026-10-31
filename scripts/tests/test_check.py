"""Test-file selection on a small repository and coverage of the real tree."""

import importlib.util
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gothic_check", ROOT / "scripts/check.py")
check = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check
SPEC.loader.exec_module(check)


def git(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args], check=True, capture_output=True, text=True
    ).stdout.strip()


@pytest.fixture
def repository(tmp_path):
    files = {
        ".gitignore": "ignored.py\n",
        "lib/pyproject.toml": (
            '[project]\nname = "fixture-lib"\nversion = "0.0.0"\n'
            '[tool.uv.build-backend]\nmodule-name = "libpkg"\n'
        ),
        "app/pyproject.toml": (
            '[project]\nname = "fixture-app"\nversion = "0.0.0"\n'
            'dependencies = ["fixture-lib"]\n'
            '[tool.uv.sources]\nfixture-lib = {path = "../lib", editable = true}\n'
            '[tool.uv.build-backend]\nmodule-name = "apppkg"\n'
        ),
        "lib/uv.lock": "version = 1\n",
        "app/uv.lock": "version = 1\n",
        "lib/src/libpkg/__init__.py": "",
        "lib/src/libpkg/value.py": "VALUE = 1\n",
        "lib/src/libpkg/bridge.py": "from . import value\nVALUE = value.VALUE\n",
        "lib/src/libpkg/other.py": "READY = True\n",
        "lib/tests/test_direct.py": "from libpkg.value import VALUE\n",
        "lib/tests/test_from_package.py": "from libpkg import value\n",
        "lib/tests/test_transitive.py": "from libpkg.bridge import VALUE\n",
        "lib/tests/test_other.py": "from libpkg.other import READY\n",
        "lib/tests/test_unused.py": "def test_unused(): pass\n",
        "app/src/apppkg/__init__.py": "",
        "app/src/apppkg/service.py": "from libpkg.bridge import VALUE\n",
        "app/src/apppkg/local.py": "from .helpers import flag\n",
        "app/src/apppkg/helpers.py": "flag = True\n",
        "app/tests/fake_game.py": "def play(): pass\n",
        "app/tests/test_cross.py": "from apppkg.service import VALUE\n",
        "app/tests/test_helper.py": "from fake_game import play\n",
        "app/tests/test_resource.py": (
            'RESOURCE = "data.json"\nSCRIPT = "special-tool.sh"\n'
        ),
        "app/tests/nested/conftest.py": "FLAG = True\n",
        "app/tests/nested/test_nested.py": "from apppkg.local import flag\n",
        "app/tests/nested/test_untouched.py": "def test_untouched(): pass\n",
        "app/data.json": "{}\n",
        "app/unread.txt": "unreferenced\n",
        "scripts/special-tool.sh": "#!/bin/sh\nexit 0\n",
    }
    for name, text in files.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    git(tmp_path, "init", "-q", "-b", "main")
    git(tmp_path, "add", ".")
    git(tmp_path, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
        "commit", "-qm", "Initial fixture")
    git(tmp_path, "update-ref", "refs/remotes/origin/main", "HEAD")
    command = ("uv", "run", "--locked", "--with", "pytest-timeout", *check.PYTEST)
    groups = (check.Group("lib", "lib", command), check.Group("app", "app", command))
    return tmp_path, groups


def selection(repository, *paths, event="pull_request"):
    root, groups = repository
    return check.select_groups(root, paths, event=event, groups=groups)


def selected_files(matrix):
    return {path for group in matrix for path in group["files"]}


def all_files(repository):
    root, groups = repository
    return {path for paths in check.discover_tests(root, groups).values() for path in paths}


def test_changed_test_selects_itself(repository):
    path = "lib/tests/test_unused.py"
    matrix = selection(repository, path)
    assert selected_files(matrix) == {path}
    assert [group["name"] for group in matrix] == ["lib"]
    assert "tests/test_unused.py" in matrix[0]["command"]
    assert matrix[0]["whole_group"] is None
    assert set(matrix[0]["reasons"]) == {path}
    assert matrix[0]["reasons"][path]


def test_imports_cross_relative_modules_and_project_boundary(repository):
    matrix = selection(repository, "lib/src/libpkg/value.py")
    assert selected_files(matrix) == {
        "lib/tests/test_direct.py",
        "lib/tests/test_from_package.py",
        "lib/tests/test_transitive.py",
        "app/tests/test_cross.py",
    }


def test_relative_named_import_follows_intermediate_source(repository):
    assert selected_files(selection(repository, "app/src/apppkg/helpers.py")) == {
        "app/tests/nested/test_nested.py"
    }

    root, _ = repository
    (root / "app/src/apppkg/helpers.py").unlink()
    git(root, "add", "-u")
    assert selected_files(selection(repository, "app/src/apppkg/helpers.py")) == {
        "app/tests/nested/test_nested.py"
    }


def test_conftest_selects_only_tests_below_its_directory(repository):
    assert selected_files(selection(repository, "app/tests/nested/conftest.py")) == {
        "app/tests/nested/test_nested.py", "app/tests/nested/test_untouched.py"
    }
    root, _ = repository
    (root / "app/tests/nested/conftest.py").write_text("from apppkg.helpers import flag\n")
    assert selected_files(selection(repository, "app/src/apppkg/helpers.py")) == {
        "app/tests/nested/test_nested.py", "app/tests/nested/test_untouched.py"
    }


def test_test_helper_selects_its_importers(repository):
    assert selected_files(selection(repository, "app/tests/fake_game.py")) == {
        "app/tests/test_helper.py"
    }
    root, _ = repository
    (root / "app/tests/test_helper.py").write_text(
        'import importlib.util\nfrom pathlib import Path\n'
        'SCRIPT = Path(__file__).with_name("fake_game.py")\n'
        'spec = importlib.util.spec_from_file_location("fixture", SCRIPT)\n'
    )
    assert selected_files(selection(repository, "app/tests/fake_game.py")) == {
        "app/tests/test_helper.py"
    }
    (root / "app/tests/test_helper.py").unlink()
    assert selection(repository, "app/tests/test_helper.py") == []


@pytest.mark.parametrize("filename", ["pyproject.toml", "uv.lock"])
def test_project_config_selects_whole_group(repository, filename, monkeypatch, capsys):
    expected = {path for path in all_files(repository) if path.startswith("app/")}
    matrix = selection(repository, f"app/{filename}")
    assert selected_files(matrix) == expected
    assert matrix[0]["whole_group"]
    assert matrix[0]["command"] == "uv run --locked --with pytest-timeout pytest -q -rs --timeout=120 --timeout-method=signal"
    assert matrix[0]["reasons"] == {}

    root, groups = repository
    monkeypatch.setattr(check, "__file__", str(root / "scripts/check.py"))
    monkeypatch.setattr(check, "GROUPS", groups)
    monkeypatch.setattr(check, "changed_paths", lambda *args, **kwargs: [f"app/{filename}"])
    monkeypatch.setattr(sys, "argv", ["check.py", "plan"])
    assert check.main() == 0
    lines = capsys.readouterr().out.splitlines()
    assert len(lines) == 1
    assert lines[0].startswith("app: whole group: ")
    assert filename in lines[0]
    assert "tests/" not in lines[0]


@pytest.mark.parametrize("filename", ["pyproject.toml", "uv.lock"])
def test_path_dependency_config_selects_all_dependent_groups(repository, filename):
    matrix = selection(repository, f"lib/{filename}")
    assert selected_files(matrix) == all_files(repository)
    assert all(group["whole_group"] for group in matrix)
    assert all(group["command"] == "uv run --locked --with pytest-timeout pytest -q -rs --timeout=120 --timeout-method=signal" for group in matrix)


def test_non_python_resource_matches_filename(repository):
    assert selected_files(selection(repository, "app/data.json")) == {
        "app/tests/test_resource.py"
    }


@pytest.mark.parametrize("path", ["app/unread.txt", "unreferenced.sh"])
def test_unreferenced_path_selects_nothing_and_explains(repository, capsys, path):
    assert selection(repository, path) == []
    assert path in capsys.readouterr().err


def test_external_script_selects_referencing_tests(repository):
    assert selected_files(selection(repository, "scripts/special-tool.sh")) == {
        "app/tests/test_resource.py"
    }


def test_external_file_matches_full_path(repository):
    root, _ = repository
    path = root / "app/tests/test_resource.py"
    path.write_text('SCRIPT = "tools/run-special"\n')
    assert selected_files(selection(repository, "tools/run-special")) == {
        "app/tests/test_resource.py"
    }


def test_docs_and_images_select_nothing(repository):
    root, _ = repository
    (root / "app/tests/test_resource.py").write_text(
        'REFERENCES = "docs/x.md README.md docs/sample.py app/README.md '
        'image.png image.jpg icon.svg reports/frame.npz"\n'
    )
    assert selection(repository, "docs/x.md", "README.md", "docs/sample.py",
                     "app/README.md", "image.png", "image.jpg", "icon.svg",
                     "reports/frame.npz") == []


@pytest.mark.parametrize("path", ["scripts/check.py", ".github/workflows/tests.yml"])
def test_selector_or_workflow_change_selects_everything(repository, path):
    matrix = selection(repository, path)
    assert selected_files(matrix) == all_files(repository)
    assert all(group["whole_group"] for group in matrix)
    assert all(group["command"] == "uv run --locked --with pytest-timeout pytest -q -rs --timeout=120 --timeout-method=signal" for group in matrix)
    assert all(group["reasons"] == {} for group in matrix)


def test_workflow_dispatch_selects_everything(repository):
    matrix = selection(repository, event="workflow_dispatch")
    assert selected_files(matrix) == all_files(repository)
    assert all(group["whole_group"] for group in matrix)
    assert all(group["command"] == "uv run --locked --with pytest-timeout pytest -q -rs --timeout=120 --timeout-method=signal" for group in matrix)


def test_more_than_sixty_percent_expands_to_whole_group(repository):
    paths = sorted(path for path in all_files(repository) if path.startswith("app/"))
    partial = selection(repository, *paths[:3])
    assert selected_files(partial) == set(paths[:3])
    assert partial[0]["whole_group"] is None
    assert all(str(Path(path).relative_to("app")) in partial[0]["command"] for path in paths[:3])
    whole = selection(repository, *paths[:4])
    assert selected_files(whole) == set(paths)
    assert whole[0]["whole_group"]
    assert whole[0]["command"] == "uv run --locked --with pytest-timeout pytest -q -rs --timeout=120 --timeout-method=signal"
    assert whole[0]["reasons"] == {}


def test_local_changes_include_commits_index_worktree_and_untracked(repository):
    root, _ = repository
    (root / "app/src/apppkg/local.py").write_text("flag = False\n")
    git(root, "add", "app/src/apppkg/local.py")
    git(root, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
        "commit", "-qm", "Change source")
    (root / "app/data.json").write_text('{"changed": true}\n')
    git(root, "add", "app/data.json")
    (root / "lib/src/libpkg/value.py").write_text("VALUE = 2\n")
    (root / "app/tests/test_new.py").write_text("def test_new(): pass\n")
    (root / "ignored.py").write_text("ignored = True\n")
    assert set(check.changed_paths(root, working_tree=True)) == {
        "app/src/apppkg/local.py", "app/data.json", "lib/src/libpkg/value.py",
        "app/tests/test_new.py",
    }
    assert check.changed_paths(root) == ["app/src/apppkg/local.py"]


def test_real_docs_select_nothing():
    assert check.select_groups(ROOT, ["docs/x.md", "README.md"]) == []


def test_real_game_change_selects_server_importers():
    matrix = check.select_groups(ROOT, ["server/src/gothic_ai/game.py"])
    files = selected_files(matrix)
    assert {group["name"] for group in matrix} == {"server", "scripts"}
    assert "scripts/tests/test_body_live_capture.py" in files
    assert "server/tests/test_memprobe.py" in files
    assert "server/tests/test_neural_transitions.py" in files
    assert "server/tests/test_body_control.py" in files
    total = check.discover_tests(ROOT)["server"]
    server_files = {path for path in files if path.startswith("server/")}
    if len(server_files) != len(total):
        assert len(server_files) <= 0.6 * len(total)
        assert "server/tests/test_protocol.py" not in files


def test_real_bridge_change_selects_only_text_references():
    path = "OpenGothic/ai_bridge/aibridge.cpp"
    expected = {
        test for tests in check.discover_tests(ROOT).values() for test in tests
        if path in (ROOT / test).read_text() or "aibridge.cpp" in (ROOT / test).read_text()
    }
    assert expected
    assert selected_files(check.select_groups(ROOT, [path])) == expected


def test_real_selector_change_selects_every_group():
    expected = {
        path for tests in check.discover_tests(ROOT).values() for path in tests
    }
    matrix = check.select_groups(ROOT, ["scripts/check.py"])
    assert selected_files(matrix) == expected
    assert {group["name"] for group in matrix if group["platform"] == "macos-15"} == {"shorts"}
    assert all(group["platform"] == "ubuntu-24.04" for group in matrix if group["name"] != "shorts")
    assert all("--with pytest-timeout" in group["command"] for group in matrix)
    assert all("--timeout=120 --timeout-method=signal" in group["command"] for group in matrix)
    assert all("faulthandler_timeout" not in group["command"] for group in matrix)
    assert "--editable ." in next(group["command"] for group in matrix if group["name"] == "body-service")
    assert "--editable ../body-service" in next(group["command"] for group in matrix if group["name"] == "scripts")


def test_every_tracked_test_is_grouped_once_or_explicitly_excluded():
    tracked = {
        path for path in git(ROOT, "ls-files").splitlines()
        if Path(path).name.startswith("test_") and path.endswith(".py")
    }
    grouped = [path for tests in check.discover_tests(ROOT).values() for path in tests]
    assert len(grouped) == len(set(grouped))
    exclusions = set(check.EXCLUSIONS)
    assert set(grouped).isdisjoint(exclusions)
    assert tracked == (set(grouped) & tracked) | exclusions
    assert all(reason.strip() for reason in check.EXCLUSIONS.values())
