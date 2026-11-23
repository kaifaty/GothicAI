#!/usr/bin/env python3
"""Check author and committer addresses of the commits a branch adds.

Two rules:
- the project's own account commits only as OWN_EMAIL (its id-form noreply);
- a GitHub noreply address must carry the account id: the old `login@users.noreply.github.com`
  form is bound to whatever account owns that login, so a guessed one credits a stranger.

Commits GitHub made itself (committer noreply@github.com: merges and edits in the web UI) only warn:
their author comes from the account's email settings, not from the agent.

    python3 scripts/commit_identity.py check [--base origin/main] [--head HEAD]
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass

OWN_EMAIL = "11953142+kaifaty@users.noreply.github.com"
OWN_NAMES = {"kaifat", "kaifaty"}
GITHUB_COMMITTER = "noreply@github.com"
NOREPLY_DOMAIN = "@users.noreply.github.com"
NOREPLY_WITH_ID = re.compile(r"^\d+\+[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?@users\.noreply\.github\.com$")


@dataclass(frozen=True)
class Commit:
    sha: str
    subject: str
    author_name: str
    author_email: str
    committer_name: str
    committer_email: str


def identity_problem(name: str, email: str) -> str | None:
    """Why this name and address must not sign a commit, or None when they may."""
    lowered = email.casefold()
    if lowered.endswith(NOREPLY_DOMAIN) and not NOREPLY_WITH_ID.match(email):
        return (
            f"адрес {email} — noreply без id аккаунта; GitHub привяжет коммит к аккаунту с этим логином"
        )
    if name.casefold() in OWN_NAMES and lowered != OWN_EMAIL:
        return f"аккаунт проекта подписывает коммиты только адресом {OWN_EMAIL}, а здесь {email}"
    return None


def commit_problems(commit: Commit) -> tuple[list[str], list[str]]:
    """Errors and warnings for one commit."""
    errors: list[str] = []
    warnings: list[str] = []
    by_github = commit.committer_email.casefold() == GITHUB_COMMITTER
    roles = [("автор", commit.author_name, commit.author_email)]
    if not by_github:
        roles.append(("коммитер", commit.committer_name, commit.committer_email))
    for role, name, email in roles:
        problem = identity_problem(name, email)
        if problem:
            (warnings if by_github else errors).append(f"{role}: {problem}")
    return errors, warnings


def branch_commits(base: str, head: str) -> list[Commit]:
    fmt = "%H%x00%s%x00%an%x00%ae%x00%cn%x00%ce%x1e"
    out = subprocess.run(
        ["git", "log", f"--format={fmt}", f"{base}..{head}"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    commits = []
    for record in out.split("\x1e"):
        record = record.strip("\n")
        if record:
            commits.append(Commit(*record.split("\x00")))
    return commits


def report(level: str, title: str, text: str) -> None:
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::{level} title={title}::{text}")
    else:
        print(f"{level}: {title}: {text}")


def check(base: str, head: str) -> int:
    commits = branch_commits(base, head)
    failed = 0
    for commit in commits:
        errors, warnings = commit_problems(commit)
        short = f"{commit.sha[:9]} {commit.subject}"
        for text in errors:
            report("error", "Адрес в коммите", f"{short}: {text}")
        for text in warnings:
            report("warning", "Адрес в коммите GitHub", f"{short}: {text}")
        failed += bool(errors)
    if failed:
        print(
            f"\nКоммитов с чужим адресом: {failed} из {len(commits)}. Перепишите их в ветке PR:\n"
            f"  git -c user.name=Kaifat -c user.email={OWN_EMAIL} \\\n"
            f"    rebase -r {base} --exec 'git commit --amend --no-edit --reset-author'\n"
            "  git push --force-with-lease"
        )
        return 1
    print(f"Адреса в {len(commits)} коммитах ветки в порядке.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("check", help="check the commits between base and head")
    run.add_argument("--base", default="origin/main")
    run.add_argument("--head", default="HEAD")
    args = parser.parse_args(argv)
    return check(args.base, args.head)


if __name__ == "__main__":
    sys.exit(main())
