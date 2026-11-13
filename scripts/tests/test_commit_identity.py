"""Commit address check on a tiny synthetic repository."""

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gothic_commit_identity", ROOT / "scripts/commit_identity.py")
identity = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = identity
SPEC.loader.exec_module(identity)

OWN = identity.OWN_EMAIL


def git(root: Path, *args: str, env: dict | None = None) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True, env=env
    ).stdout


def commit(root: Path, name: str, email: str, committer: tuple[str, str] | None = None) -> None:
    cname, cemail = committer or (name, email)
    env = {
        "PATH": "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin",
        "HOME": str(root),
        "GIT_AUTHOR_NAME": name,
        "GIT_AUTHOR_EMAIL": email,
        "GIT_COMMITTER_NAME": cname,
        "GIT_COMMITTER_EMAIL": cemail,
    }
    git(root, "commit", "-q", "--allow-empty", "-m", f"by {email}", env=env)


@pytest.fixture
def repo(tmp_path, monkeypatch):
    git(tmp_path, "init", "-q")
    commit(tmp_path, "Kaifat", OWN)
    git(tmp_path, "branch", "base")
    monkeypatch.chdir(tmp_path)
    return tmp_path


@pytest.mark.parametrize(
    ("name", "email", "bad"),
    [
        ("Kaifat", OWN, False),
        ("kaifaty", OWN, False),
        ("Kaifat", "kaifat@users.noreply.github.com", True),
        ("Kaifat", "kaifaty@users.noreply.github.com", True),
        ("Kaifat", "someone@example.com", True),
        ("Denis", "d@example.com", False),
        ("Denis", "4670129+ffring@users.noreply.github.com", False),
        ("Denis", "ffring@users.noreply.github.com", True),
    ],
)
def test_identity_problem(name, email, bad):
    assert (identity.identity_problem(name, email) is not None) is bad


def test_clean_branch_passes(repo, capsys):
    commit(repo, "Kaifat", OWN)
    commit(repo, "Denis", "d@example.com")
    assert identity.check("base", "HEAD") == 0
    assert "в 2 коммитах" in capsys.readouterr().out


def test_guessed_noreply_fails(repo, capsys):
    commit(repo, "Kaifat", "kaifat@users.noreply.github.com")
    assert identity.check("base", "HEAD") == 1
    assert "noreply без id" in capsys.readouterr().out


def test_wrong_committer_fails(repo):
    commit(repo, "Kaifat", OWN, committer=("Kaifat", "someone@example.com"))
    assert identity.check("base", "HEAD") == 1


def test_commit_made_by_github_only_warns(repo, capsys):
    commit(repo, "Kaifat", "someone@example.com", committer=("GitHub", "noreply@github.com"))
    assert identity.check("base", "HEAD") == 0
    assert "warning" in capsys.readouterr().out
