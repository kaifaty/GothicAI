"""Test-file selection on a small repository and coverage of the real tree."""

import importlib.util
import json
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


def test_relative_named_import_follows_intermediate_source(repository, monkeypatch):
    parsed = []
    original_parse = check.ast.parse

    def parse(source, filename, *args, **kwargs):
        parsed.append(filename)
        return original_parse(source, filename, *args, **kwargs)

    monkeypatch.setattr(check.ast, "parse", parse)
    assert selected_files(selection(repository, "app/src/apppkg/helpers.py")) == {
        "app/tests/nested/test_nested.py"
    }
    assert parsed and all(path.startswith("app/") for path in parsed)

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


def test_import_cache_preserves_selection_after_edits_and_corruption(repository, monkeypatch):
    root, _ = repository
    cache = root / ".git/gothic-check-cache.json"
    changed = "app/src/apppkg/helpers.py"
    expected = {"app/tests/nested/test_nested.py"}
    assert selected_files(selection(repository, changed)) == expected
    assert cache.is_file()
    original_parse = check.parse_imports
    parsed = []

    def parse(source, filename):
        parsed.append(filename)
        return original_parse(source, filename)

    monkeypatch.setattr(check, "parse_imports", parse)
    assert selected_files(selection(repository, changed)) == expected
    assert parsed == []

    # The same blob in two packages must keep each package's relative imports.
    (root / "lib/src/libpkg/helpers.py").write_text("flag = 2\n")
    (root / "lib/src/libpkg/local.py").write_text("from .helpers import flag\n")
    (root / "lib/tests/test_local.py").write_text("from libpkg.local import flag\n")
    assert selected_files(selection(repository, "lib/src/libpkg/helpers.py")) == {
        "lib/tests/test_local.py"
    }

    importer = root / "app/tests/nested/test_nested.py"
    importer.write_text("import json\nfrom apppkg.service import VALUE\n")
    assert selection(repository, changed) == []
    assert "app/tests/nested/test_nested.py" in parsed
    git(root, "add", "app/tests/nested/test_nested.py")
    parsed.clear()
    assert selection(repository, changed) == []
    assert parsed == []

    untracked = root / "app/tests/test_new.py"
    untracked.write_text("from apppkg.helpers import flag\n")
    assert selected_files(selection(repository, changed)) == {"app/tests/test_new.py"}
    untracked.write_text("import json\n")
    assert selection(repository, changed) == []

    cache.write_text("{broken")
    parsed.clear()
    assert selection(repository, changed) == []
    assert parsed
    assert json.loads(cache.read_text())["version"] == 1


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


@pytest.mark.parametrize("parts", [
    ("tools", "run-special"),
    ("lib", "src", "libpkg", "unimported.py"),
    ("docs", "style.md"),
])
def test_external_file_matches_full_path(repository, parts):
    root, _ = repository
    changed = "/".join(parts)
    path = root / "app/tests/test_resource.py"
    path.write_text(f"RESOURCE = {changed!r}\n")
    (root / "lib/tests/test_unused.py").write_text(f"RESOURCE = {parts[-1]!r}\n")
    expected = {"app/tests/test_resource.py"}
    if parts[0] == "tools":
        expected.add("lib/tests/test_unused.py")
    assert selected_files(selection(repository, changed)) == expected


def test_docs_and_images_select_nothing(repository):
    root, _ = repository
    (root / "app/tests/test_resource.py").write_text(
        'REFERENCES = "x.md sample.py notes.md image.png image.jpg icon.svg frame.npz"\n'
    )
    assert selection(repository, "/".join(("docs", "x.md")), "README.md", "docs/sample.py",
                     "app/notes.md", "images/image.png", "images/image.jpg", "images/icon.svg",
                     "reports/frame.npz") == []


def test_scripts_partition_follows_transitive_imports_and_keeps_selections(repository):
    root, original_groups = repository
    files = {
        "scripts/backend.py": "import torch\nfrom libpkg.other import READY\n",
        "scripts/tests/test_backend.py": 'import backend\nRESOURCE = "config.json"\n',
        "scripts/tests/test_optional.py": 'import pytest\npytest.importorskip("torch")\n',
        "scripts/tests/test_body.py": "import gothic_body.schema\n",
        "scripts/tests/test_light.py": 'import json\nRESOURCE = "config.json"\n',
        "scripts/tests/conftest.py": "FLAG = True\n",
    }
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    command = ("uv", "run", "--no-project", *check.PYTEST)
    groups = (*original_groups,
              check.Group("scripts", "scripts", command, dependencies=("lib",)),
              check.Group("scripts-torch", "scripts", command, dependencies=("scripts",)))
    repository = root, groups
    tests = check.discover_tests(root, groups)
    light = {"scripts/tests/test_light.py"}
    heavy = {
        "scripts/tests/test_backend.py", "scripts/tests/test_optional.py", "scripts/tests/test_body.py"
    }
    assert set(tests["scripts"]) == light
    assert set(tests["scripts-torch"]) == heavy
    assert selected_files(selection(repository, "scripts/backend.py")) == {
        "scripts/tests/test_backend.py"
    }
    assert selected_files(selection(repository, "lib/src/libpkg/other.py")) == {
        "lib/tests/test_other.py", "scripts/tests/test_backend.py"
    }
    assert selected_files(selection(repository, "scripts/config.json")) == {
        "scripts/tests/test_light.py", "scripts/tests/test_backend.py"
    }
    matrix = selection(repository, "scripts/tests/conftest.py")
    assert selected_files(matrix) == light | heavy
    assert all(group["whole_group"] for group in matrix)
    for group in matrix:
        members = light if group["name"] == "scripts" else heavy
        assert group["command"].split()[-len(members):] == sorted(
            str(Path(path).relative_to("scripts")) for path in members
        )
        assert not any(str(Path(path).relative_to("scripts")) in group["command"]
                       for path in (light | heavy) - members)
    assert selected_files(selection(repository, "scripts/tests/test_light.py")) == light
    assert selected_files(selection(repository, "scripts/tests/test_optional.py")) == {
        "scripts/tests/test_optional.py"
    }


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
    assert check.select_groups(ROOT, ["/".join(("docs", "x.md"))]) == []


@pytest.mark.parametrize("parts, expected", [
    (("server", "src", "gothic_ai", "gamedata", "base.py"), {
        "launcher/core/tests/test_checks.py"
    }),
    (("scripts", "ryzen_game.py"), {
        "launcher/core/tests/test_commands.py"
    }),
    (("docs", "content-style.md"), {
        "scripts/content/test_city_set.py", "scripts/content/test_generate.py",
        "scripts/content/test_generate_run.py", "scripts/content/test_import_imagegen.py",
    }),
])
def test_real_full_path_references_cross_group_boundaries(parts, expected):
    files = selected_files(check.select_groups(ROOT, ["/".join(parts)]))
    assert expected <= files
    if parts[0] == "docs":
        assert files == expected


@pytest.mark.parametrize("filename", ["game.py", "voice.py"])
def test_real_game_change_selects_server_importers(filename):
    matrix = check.select_groups(ROOT, ["/".join(("server", "src", "gothic_ai", filename))])
    files = selected_files(matrix)
    assert {group["name"] for group in matrix} == {"server", "scripts-torch"}
    assert "scripts/tests/test_body_live_capture.py" in files
    assert "server/tests/test_memprobe.py" in files
    assert "server/tests/test_neural_transitions.py" in files
    assert "server/tests/test_body_control.py" in files
    total = check.discover_tests(ROOT)["server"]
    assert files == set(total) | {"scripts/tests/test_body_live_capture.py"}


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
    light = next(group["command"] for group in matrix if group["name"] == "scripts")
    heavy = next(group["command"] for group in matrix if group["name"] == "scripts-torch")
    assert "--torch-backend" not in light and "--with torch" not in light and "../body-service" not in light
    assert "--torch-backend cpu" in heavy and "--editable ../body-service" in heavy
    assert not any(group["name"].startswith("npc-body-") for group in matrix)


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
