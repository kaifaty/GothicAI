"""Check of the files a branch adds (scripts/large_files.py) on a tiny synthetic repository."""

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gothic_large_files", ROOT / "scripts/large_files.py")
large = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = large
SPEC.loader.exec_module(large)


def git(root: Path, *args: str) -> str:
    env = {
        "PATH": "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin",
        "HOME": str(root),
        "GIT_AUTHOR_NAME": "Kaifat",
        "GIT_AUTHOR_EMAIL": "11953142+kaifaty@users.noreply.github.com",
        "GIT_COMMITTER_NAME": "Kaifat",
        "GIT_COMMITTER_EMAIL": "11953142+kaifaty@users.noreply.github.com",
    }
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True, env=env
    ).stdout


def add(root: Path, path: str, size: int, message: str) -> None:
    file = root / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(path.encode() * (size // len(path)) + b"x" * (size % len(path)))
    git(root, "add", path)
    git(root, "commit", "-q", "-m", message)


@pytest.fixture
def repo(tmp_path, monkeypatch):
    git(tmp_path, "init", "-q")
    add(tmp_path, "reports/old.mp4", large.LIMIT + 1, "an old big clip")
    git(tmp_path, "branch", "base")
    monkeypatch.chdir(tmp_path)
    return tmp_path


@pytest.mark.parametrize(
    ("path", "size", "bad"),
    [
        ("reports/x/shot.jpg", 900_000, False),
        ("reports/x/clip.mp4", large.LIMIT, False),
        ("reports/x/clip.mp4", large.LIMIT + 1, True),
        ("data/fir.glb", 10, True),
        ("data/FIR.DDS", 10, True),
        ("scene.blend", 10, True),
        ("scene.blend1", 10, True),
        ("docs/notes.md", 4_000, False),
    ],
)
def test_file_problem(path, size, bad):
    assert (large.file_problem(large.Blob(path, size)) is not None) is bad


def test_small_files_pass_and_old_ones_are_not_counted(repo, capsys):
    add(repo, "reports/x/shot.jpg", 50_000, "a frame for a note")
    add(repo, "docs/a note.md", 2_000, "a note with a space in its name")
    assert large.check("base", "HEAD") == 0
    assert "Новых файлов в ветке: 2" in capsys.readouterr().out


def test_big_clip_fails(repo, capsys):
    add(repo, ".bench-out/fly.mp4", large.LIMIT + 10, "a clip by mistake")
    assert large.check("base", "HEAD") == 1
    assert ".bench-out/fly.mp4" in capsys.readouterr().out


def test_file_deleted_later_in_the_branch_still_fails(repo):
    add(repo, "data/fir.glb", 100, "a mesh by mistake")
    git(repo, "rm", "-q", "data/fir.glb")
    git(repo, "commit", "-q", "-m", "remove it again")
    assert large.check("base", "HEAD") == 1


def test_empty_branch_passes(repo):
    assert large.check("base", "HEAD") == 0
