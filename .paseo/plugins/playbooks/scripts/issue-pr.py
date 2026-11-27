#!/usr/bin/env python3
import json
import os
import subprocess
import sys
from pathlib import Path


REPO = "kaifaty/GothicAI"
REPO_URL = f"https://github.com/{REPO}"


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def gh_json(*args):
    return json.loads(output("gh", *args, "--repo", REPO))


def existing_pr(branch):
    prs = gh_json("pr", "list", "--head", branch, "--base", "main", "--state", "open",
                  "--json", "url,isCrossRepository")
    return next((pr["url"] for pr in prs if not pr["isCrossRepository"]), None)


def name_issue_workspace(issue):
    worktree = Path.cwd()
    run_id = os.environ.get("PLAYBOOK_RUN_ID")
    if run_id != worktree.name:
        return
    try:
        run = json.loads((worktree.parent.parent / "runs" / f"{run_id}.json").read_text())
        if run.get("workspaceCreated") is not True or run["worktree"]["path"] != str(worktree):
            return
        title = f"#{issue['number']} · {issue['title']}"[:100]
        subprocess.run(["paseo", "workspace", "rename", run["workspaceId"], title], check=True)
    except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Could not label the playbook workspace: {error}", file=sys.stderr)


def handle_issue():
    number = os.environ["PLAYBOOK_INPUT_ISSUE_NUMBER"]
    if not number.isascii() or not number.isdecimal() or int(number) < 1:
        sys.exit("Expected a positive issue number.")
    issue = gh_json("issue", "view", number, "--json", "number,title,state,url,labels,closedByPullRequestsReferences")
    if issue["url"] != f"{REPO_URL}/issues/{number}":
        sys.exit("Expected a GothicAI issue.")
    branch = f"paseo/issue-{number}"
    step = os.environ["PLAYBOOK_STEP_ID"]
    if step not in ("inspect", "publish"):
        sys.exit("Unknown playbook step.")

    url = issue["url"] if issue["state"] == "CLOSED" else existing_pr(branch)
    if not url:
        for pr in issue["closedByPullRequestsReferences"]:
            if output("gh", "pr", "view", pr["url"], "--json", "state", "--jq", ".state") == "OPEN":
                url = pr["url"]
                break
    if step == "inspect":
        if url:
            return {"outcome": "handled", "summary": f"Issue #{number} already handled: {url}"}
        name_issue_workspace(issue)
        return {"outcome": "ready", "summary": issue["url"]}
    if not url:
        if output("git", "status", "--porcelain"):
            sys.exit("The worktree contains uncommitted changes.")
        if not output("git", "diff", "--name-only", "origin/main...HEAD"):
            sys.exit("No committed fix to publish.")
        report = os.environ["PLAYBOOK_INPUT_REPORT"].strip()
        if not report:
            sys.exit("Missing verification report.")
        automatic = any(label["name"] == "долг-авто" for label in issue["labels"])
        reference = "Refs" if automatic else "Closes"
        title = output("git", "log", "-1", "--format=%s")
        subprocess.run(["git", "push", f"{REPO_URL}.git", f"HEAD:refs/heads/{branch}"], check=True)
        created = subprocess.run(
            ["gh", "pr", "create", "--repo", REPO, "--base", "main", "--head", branch,
             "--title", title, "--body-file", "-"],
            input=f"{report}\n\n{reference} #{number}\n", text=True,
        )
        url = existing_pr(branch)
        if not url:
            created.check_returncode()
            sys.exit("Could not verify the published pull request.")
    return {"summary": url, "outputs": {"url": url}}


if __name__ == "__main__":
    print(json.dumps(handle_issue()))
